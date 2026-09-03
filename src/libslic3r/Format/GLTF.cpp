// glTF 2.0 / GLB geometry import.
//
// One file becomes exactly one ModelObject with one ModelVolume per (node, primitive) pair.
// Node transforms are baked into the vertices (see docs/superpowers/plans/2026-09-02-glb-import.md
// section 3.3 for why), the glTF Y-up frame is rotated into the slicer's Z-up frame, and one
// glTF unit is one millimetre - a genuinely metre-authored file then trips the slicer's existing
// "this object is tiny, scale it?" prompt instead of needing a rule of its own.

#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../format.hpp"

#include "GLTF.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

// Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

// This translation unit owns the cgltf implementation. Nothing else in the tree may define
// CGLTF_IMPLEMENTATION. cgltf.h is C99 compiled as C++ here; the pragmas keep MSVC quiet about
// the vendored code without lowering the warning level for our own.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127 4189 4244 4245 4267 4456 4457 4701 4703)
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// EXT_meshopt_compression: cgltf recognises the extension and hands us the compressed span plus
// the decoded layout, but does not decode. Only meshoptimizer's decoder is vendored.
#include <meshoptimizer/meshoptimizer.h>

namespace Slic3r {

namespace {

// --- limits -----------------------------------------------------------------------------
// Every one of these produces a named error rather than an out-of-memory crash. The reader sits
// behind the phone upload endpoint (RemoteHub::spool_upload), so nothing may be sized purely by
// a number read out of the file.
constexpr uint64_t MAX_GLTF_FILE      = 512ull * 1024 * 1024;  // whole .glb / .gltf, and buffers
constexpr size_t   MAX_GLTF_TRIANGLES = 20u * 1000 * 1000;     // across the whole import
constexpr size_t   MAX_GLTF_VERTICES  = 3 * MAX_GLTF_TRIANGLES;
constexpr size_t   MAX_GLTF_VOLUMES   = 2000;
constexpr size_t   MAX_NODE_DEPTH     = 256;                   // guards a pathological node tree

// Two messages used from several places. Kept as functions so the literal stays inside _L() for
// the translation extractor.
std::string err_damaged() { return _L("This glTF file is damaged or incomplete."); }
std::string err_draco()
{
    return _L("This file uses Draco mesh compression, which Snapmaker Orca cannot read yet. "
              "Re-export it without Draco compression.");
}

// --- small helpers ----------------------------------------------------------------------

// The file stem, taken from the UTF-8 path by bytes. Deliberately not boost::filesystem: the
// object name must survive a non-ASCII path whatever the process locale happens to be.
std::string file_stem_utf8(const char *path)
{
    std::string s(path == nullptr ? "" : path);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        s = s.substr(0, dot);
    return s;
}

// glTF is right-handed +Y up, the slicer is Z-up: Rx(+90), i.e. (x, y, z) -> (x, -z, y).
// Determinant +1, so this step alone never flips triangle winding.
Matrix4d up_axis_correction()
{
    Matrix4d m = Matrix4d::Zero();
    m(0, 0) =  1.0;
    m(1, 2) = -1.0;
    m(2, 1) =  1.0;
    m(3, 3) =  1.0;
    return m;
}

// glTF baseColorFactor is linear; the filament swatches the colour dialog compares against are
// sRGB. Without this the imported colours look washed out (Stage 2 consumes the result).
float linear_to_srgb(float c)
{
    if (!(c > 0.f))
        return 0.f;
    if (c >= 1.f)
        return 1.f;
    return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.f / 2.4f) - 0.055f);
}

std::string unique_part_name(const std::string &base, std::set<std::string> &taken)
{
    if (taken.insert(base).second)
        return base;
    for (int i = 2; i < 1000000; ++i) {
        std::string candidate = base + "_" + std::to_string(i);
        if (taken.insert(candidate).second)
            return candidate;
    }
    return base;
}

// --- buffer URIs ------------------------------------------------------------------------

std::string percent_decode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char) ((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

// A sidecar buffer may only sit inside the .gltf's own directory tree. Same posture as the
// OBJ/MTL sibling handling, and it matters because the phone upload endpoint writes a file an
// attacker chose the contents of.
bool uri_is_safe_relative(const std::string &uri_raw)
{
    const std::string uri = percent_decode(uri_raw);
    if (uri.empty())
        return false;
    if (uri.front() == '/' || uri.front() == '\\')            // rooted
        return false;
    if (uri.find(':') != std::string::npos)                   // drive letter or a scheme
        return false;
    int depth = 0;
    size_t start = 0;
    while (start <= uri.size()) {
        size_t end = uri.find_first_of("/\\", start);
        if (end == std::string::npos)
            end = uri.size();
        const std::string seg = uri.substr(start, end - start);
        if (seg == "..") {
            if (--depth < 0)
                return false;
        } else if (!seg.empty() && seg != ".") {
            ++depth;
        }
        if (end == uri.size())
            break;
        start = end + 1;
    }
    return depth > 0;
}

// cgltf's file callbacks. Never cgltf_parse_file / the default fopen callbacks: fopen on Windows
// cannot open a UTF-8 path with non-ASCII characters, and non-ASCII import paths are a case this
// repo already guards for STL.
struct FileReadContext
{
    uint64_t bytes_read{0};
    bool     too_large{false};
};

cgltf_result nowide_file_read(const cgltf_memory_options *memory_options,
                              const cgltf_file_options   *file_options,
                              const char *path, cgltf_size *size, void **data)
{
    FileReadContext *ctx = file_options == nullptr ? nullptr : (FileReadContext *) file_options->user_data;

    boost::nowide::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
        return cgltf_result_file_not_found;
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    if (len < 0)
        return cgltf_result_io_error;
    const uint64_t file_size = (uint64_t) len;
    if (ctx != nullptr && (file_size > MAX_GLTF_FILE || ctx->bytes_read + file_size > MAX_GLTF_FILE)) {
        ctx->too_large = true;
        return cgltf_result_out_of_memory;
    }
    in.seekg(0, std::ios::beg);

    void *(*alloc_func)(void *, cgltf_size) =
        (memory_options != nullptr && memory_options->alloc_func != nullptr) ? memory_options->alloc_func : nullptr;
    void *buf = alloc_func != nullptr ? alloc_func(memory_options->user_data, (cgltf_size) file_size)
                                      : std::malloc((size_t) file_size == 0 ? 1 : (size_t) file_size);
    if (buf == nullptr)
        return cgltf_result_out_of_memory;
    if (file_size > 0) {
        in.read((char *) buf, (std::streamsize) file_size);
        if ((uint64_t) in.gcount() != file_size) {
            if (memory_options != nullptr && memory_options->free_func != nullptr)
                memory_options->free_func(memory_options->user_data, buf);
            else
                std::free(buf);
            return cgltf_result_io_error;
        }
    }
    if (ctx != nullptr)
        ctx->bytes_read += file_size;
    *size = (cgltf_size) file_size;
    *data = buf;
    return cgltf_result_success;
}

void nowide_file_release(const cgltf_memory_options *memory_options, const cgltf_file_options *, void *data)
{
    if (data == nullptr)
        return;
    if (memory_options != nullptr && memory_options->free_func != nullptr)
        memory_options->free_func(memory_options->user_data, data);
    else
        std::free(data);
}

// --- accessors --------------------------------------------------------------------------

// Belt and braces on top of cgltf_validate: an accessor must really be backed by bytes that are
// inside its buffer before we size an allocation from its element count.
bool accessor_data_fits(const cgltf_accessor &acc)
{
    if (acc.count == 0)
        return false;
    if (acc.buffer_view == nullptr)
        return acc.is_sparse != 0;      // a sparse accessor may have an all-zero base
    const cgltf_buffer_view &bv = *acc.buffer_view;
    if (bv.data == nullptr) {
        // Plain view: the bytes live in the buffer, at bv.offset.
        if (bv.buffer == nullptr || bv.buffer->data == nullptr)
            return false;
        if (bv.offset > bv.buffer->size || bv.size > bv.buffer->size - bv.offset)
            return false;
    }
    // A decoded view (meshopt) owns exactly bv.size bytes; either way the accessor must fit those.
    const cgltf_size stride = acc.stride != 0 ? acc.stride : 1;
    if (acc.count > (cgltf_size) -1 / stride)
        return false;
    const cgltf_size span = (acc.count - 1) * stride + stride;
    return acc.offset <= bv.size && span <= bv.size - acc.offset;
}

// Decode every EXT_meshopt_compression buffer view in place. cgltf_buffer_view::data is the hook
// cgltf documents for exactly this ("overrides buffer->data if present, filled by extensions"),
// and cgltf_free releases it with the same allocator we take it from - so nothing here leaks.
bool decode_meshopt_buffer_views(cgltf_data *data, std::string &message)
{
    uint64_t decoded_total = 0;
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
        cgltf_buffer_view &bv = data->buffer_views[i];
        if (!bv.has_meshopt_compression || bv.data != nullptr)
            continue;
        const cgltf_meshopt_compression &mc = bv.meshopt_compression;
        // Everything below is a number read out of the file, so none of it is trusted.
        if (mc.buffer == nullptr || mc.buffer->data == nullptr || mc.offset > mc.buffer->size ||
            mc.size > mc.buffer->size - mc.offset || mc.count == 0 || mc.stride == 0 ||
            mc.stride > 256 || bv.size != mc.stride * mc.count) {
            message = err_damaged();
            return false;
        }
        decoded_total += (uint64_t) bv.size;
        if ((uint64_t) bv.size > MAX_GLTF_FILE || decoded_total > MAX_GLTF_FILE) {
            message = format(_L("This glTF file expands to more than %1% MB of mesh data, which "
                                "Snapmaker Orca will not load."),
                             (unsigned long long) (MAX_GLTF_FILE / (1024 * 1024)));
            return false;
        }

        void *out = data->memory.alloc_func(data->memory.user_data, bv.size);
        if (out == nullptr) {
            message = _L("There is not enough memory to read this glTF file.");
            return false;
        }
        const unsigned char *src = (const unsigned char *) mc.buffer->data + mc.offset;
        int                  rc  = -1;
        switch (mc.mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(out, mc.count, mc.stride, src, mc.size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(out, mc.count, mc.stride, src, mc.size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(out, mc.count, mc.stride, src, mc.size);
            break;
        default:
            break;
        }
        if (rc != 0) {
            data->memory.free_func(data->memory.user_data, out);
            message = err_damaged();
            return false;
        }
        switch (mc.filter) {
        case cgltf_meshopt_compression_filter_octahedral:  meshopt_decodeFilterOct(out, mc.count, mc.stride);  break;
        case cgltf_meshopt_compression_filter_quaternion:  meshopt_decodeFilterQuat(out, mc.count, mc.stride); break;
        case cgltf_meshopt_compression_filter_exponential: meshopt_decodeFilterExp(out, mc.count, mc.stride);  break;
        default: break;
        }
        bv.data = out;   // cgltf_free() releases this
    }
    return true;
}

const cgltf_accessor *find_attribute(const cgltf_primitive &prim, cgltf_attribute_type type, int index)
{
    for (cgltf_size a = 0; a < prim.attributes_count; ++a)
        if (prim.attributes[a].type == type && prim.attributes[a].index == index)
            return prim.attributes[a].data;
    return nullptr;
}

// --- the scene walk ---------------------------------------------------------------------

struct DrawItem
{
    const cgltf_node      *node{nullptr};
    const cgltf_primitive *prim{nullptr};
    size_t                 prim_index{0};
    size_t                 prim_count{0};
};

void collect_draw_items(const cgltf_node *node, size_t depth, std::set<const cgltf_node *> &seen,
                        std::vector<DrawItem> &out)
{
    if (node == nullptr || depth > MAX_NODE_DEPTH || !seen.insert(node).second)
        return;
    if (node->mesh != nullptr) {
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p)
            out.push_back(DrawItem{node, &node->mesh->primitives[p], (size_t) p, (size_t) node->mesh->primitives_count});
    }
    for (cgltf_size c = 0; c < node->children_count; ++c)
        collect_draw_items(node->children[c], depth + 1, seen, out);
}

// One primitive's geometry, before it becomes a TriangleMesh.
struct PrimitiveGeometry
{
    indexed_triangle_set its;
    std::vector<RGBA>    vertex_colors;   // one per its.vertices entry, empty when no COLOR_0
    bool                 has_color0{false};
};

// Read `prim` and append it to `geo`, transformed by `world`. Returns false only for a damaged
// file; an empty result (all faces degenerate, say) is a success with no triangles.
bool build_primitive(const cgltf_primitive &prim, const Matrix4d &world, PrimitiveGeometry &geo,
                     size_t &out_of_range_indices, std::string &message)
{
    const cgltf_accessor *pos = find_attribute(prim, cgltf_attribute_type_position, 0);
    if (pos == nullptr || cgltf_num_components(pos->type) != 3 || !accessor_data_fits(*pos)) {
        message = err_damaged();
        return false;
    }
    const size_t vertex_count = (size_t) pos->count;
    if (vertex_count > MAX_GLTF_VERTICES) {
        message = err_damaged();
        return false;
    }

    std::vector<float> coords(vertex_count * 3, 0.f);
    if (cgltf_accessor_unpack_floats(pos, coords.data(), coords.size()) != coords.size()) {
        message = err_damaged();
        return false;
    }

    geo.its.vertices.resize(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const double x = coords[i * 3 + 0], y = coords[i * 3 + 1], z = coords[i * 3 + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            message = err_damaged();
            return false;
        }
        const Vec3d p(world(0, 0) * x + world(0, 1) * y + world(0, 2) * z + world(0, 3),
                      world(1, 0) * x + world(1, 1) * y + world(1, 2) * z + world(1, 3),
                      world(2, 0) * x + world(2, 1) * y + world(2, 2) * z + world(2, 3));
        if (!p.allFinite()) {
            message = err_damaged();
            return false;
        }
        geo.its.vertices[i] = p.cast<float>();
    }

    // COLOR_0 is a linear multiplier on baseColorFactor; Stage 2 multiplies before encoding.
    // cgltf_accessor_read_float normalises u8/u16 and fills alpha 1.0 for a VEC3 accessor.
    const cgltf_accessor *col = find_attribute(prim, cgltf_attribute_type_color, 0);
    if (col != nullptr && col->count == pos->count && accessor_data_fits(*col)) {
        geo.vertex_colors.assign(vertex_count, RGBA{1.f, 1.f, 1.f, 1.f});
        bool ok = true;
        for (size_t i = 0; i < vertex_count && ok; ++i) {
            float rgba[4] = {1.f, 1.f, 1.f, 1.f};
            ok = cgltf_accessor_read_float(col, (cgltf_size) i, rgba, 4) != 0;
            geo.vertex_colors[i] = RGBA{rgba[0], rgba[1], rgba[2], rgba[3]};
        }
        if (ok)
            geo.has_color0 = true;
        else
            geo.vertex_colors.clear();
    }

    // Indices. cgltf_accessor_unpack_indices handles u8 / u16 / u32; without an index accessor
    // the vertices are consumed in order (Models/TriangleWithoutIndices does exactly that).
    std::vector<uint32_t> indices;
    if (prim.indices != nullptr) {
        if (!accessor_data_fits(*prim.indices)) {
            message = err_damaged();
            return false;
        }
        const size_t n = (size_t) prim.indices->count;
        if (n > MAX_GLTF_VERTICES) {
            message = err_damaged();
            return false;
        }
        indices.resize(n);
        if (cgltf_accessor_unpack_indices(prim.indices, indices.data(), sizeof(uint32_t), n) != n) {
            message = err_damaged();
            return false;
        }
    } else {
        indices.resize(vertex_count);
        for (size_t i = 0; i < vertex_count; ++i)
            indices[i] = (uint32_t) i;
    }

    auto emit = [&geo, &indices, vertex_count, &out_of_range_indices](size_t a, size_t b, size_t c) {
        const uint32_t ia = indices[a], ib = indices[b], ic = indices[c];
        if (ia >= vertex_count || ib >= vertex_count || ic >= vertex_count) {
            ++out_of_range_indices;
            return;
        }
        geo.its.indices.emplace_back((int) ia, (int) ib, (int) ic);
    };

    const size_t n = indices.size();
    switch (prim.type) {
    case cgltf_primitive_type_triangles:
        for (size_t i = 0; i + 2 < n; i += 3)
            emit(i, i + 1, i + 2);
        break;
    case cgltf_primitive_type_triangle_strip:
        // The spec's winding rule: odd triangles swap their first two vertices. Get this wrong
        // and every other face of a strip is inside out.
        for (size_t i = 0; i + 2 < n; ++i) {
            if ((i & 1) == 0)
                emit(i, i + 1, i + 2);
            else
                emit(i + 1, i, i + 2);
        }
        break;
    case cgltf_primitive_type_triangle_fan:
        for (size_t i = 1; i + 1 < n; ++i)
            emit(0, i, i + 1);
        break;
    default:
        break;   // never reached: the caller filters the mode
    }
    return true;
}

} // anonymous namespace

bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message,
               const char *object_name_in, ImportstlProgressFn progressFn)
{
    info = GltfInfo();
    message.clear();
    if (path == nullptr || model == nullptr) {
        message = err_damaged();
        return false;
    }

    // 1. Read the whole file ourselves (nowide, size-capped) and hand the bytes to cgltf_parse.
    //    cgltf_parse keeps pointers into this buffer for a .glb's BIN chunk, so it must outlive
    //    the cgltf_data.
    std::vector<char> bytes;
    {
        boost::nowide::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            message = _L("The file could not be opened.");
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff len = in.tellg();
        if (len < 0) {
            message = _L("The file could not be read.");
            return false;
        }
        if ((uint64_t) len > MAX_GLTF_FILE) {
            message = format(_L("This file is %1% MB. Snapmaker Orca imports glTF files up to %2% MB."),
                             (unsigned long long) ((uint64_t) len / (1024 * 1024)),
                             (unsigned long long) (MAX_GLTF_FILE / (1024 * 1024)));
            return false;
        }
        in.seekg(0, std::ios::beg);
        bytes.resize((size_t) len);
        if (len > 0) {
            in.read(bytes.data(), (std::streamsize) len);
            if (in.gcount() != len) {
                message = _L("The file could not be read.");
                return false;
            }
        }
    }

    FileReadContext file_ctx;
    file_ctx.bytes_read = bytes.size();

    cgltf_options options {};
    options.file.read      = &nowide_file_read;
    options.file.release   = &nowide_file_release;
    options.file.user_data = &file_ctx;

    cgltf_data  *data   = nullptr;
    cgltf_result parsed = cgltf_parse(&options, bytes.data(), (cgltf_size) bytes.size(), &data);
    if (parsed != cgltf_result_success || data == nullptr) {
        switch (parsed) {
        case cgltf_result_unknown_format: message = _L("This is not a glTF or GLB file."); break;
        case cgltf_result_legacy_gltf:    message = _L("This is a glTF 1.0 file; only glTF 2.0 is supported."); break;
        case cgltf_result_out_of_memory:  message = _L("There is not enough memory to read this glTF file."); break;
        default:                          message = err_damaged(); break;
        }
        if (data != nullptr)
            cgltf_free(data);
        return false;
    }

    // Everything from here on must go through this guard so no early return leaks cgltf_data.
    struct DataGuard
    {
        cgltf_data *d;
        ~DataGuard() { if (d != nullptr) cgltf_free(d); }
    } guard{data};

    // 2. extensionsRequired, before anything sized by file content. extensionsUsed is advisory
    //    and must never cause a refusal.
    {
        std::vector<std::string> unsupported;
        bool draco = false;
        for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
            const char *ext = data->extensions_required[i];
            if (ext == nullptr)
                continue;
            const std::string name(ext);
            if (name == "KHR_mesh_quantization")
                continue;   // cgltf_accessor_unpack_floats de-quantizes for us
            if (name == "EXT_meshopt_compression")
                continue;   // decoded below, by the vendored meshoptimizer decoder
            if (name == "KHR_draco_mesh_compression")
                draco = true;
            unsupported.push_back(name);
        }
        info.unsupported_extensions = unsupported;
        if (draco) {
            message = err_draco();
            return false;
        }
        if (!unsupported.empty()) {
            std::string list;
            for (const std::string &n : unsupported)
                list += (list.empty() ? "" : ", ") + n;
            message = format(_L("This file needs the glTF extension \"%1%\", which Snapmaker Orca does not support."), list);
            return false;
        }
    }

    // 3. Sidecar and data: buffers. cgltf resolves the URIs; we vet them first and cap the total.
    {
        uint64_t declared = 0;
        for (cgltf_size i = 0; i < data->buffers_count; ++i) {
            declared += (uint64_t) data->buffers[i].size;
            if (declared > MAX_GLTF_FILE) {
                message = format(_L("This glTF file declares more than %1% MB of mesh data, which Snapmaker Orca "
                                    "will not load."),
                                 (unsigned long long) (MAX_GLTF_FILE / (1024 * 1024)));
                return false;
            }
            const char *uri = data->buffers[i].uri;
            if (uri == nullptr || data->buffers[i].data != nullptr)
                continue;
            const std::string u(uri);
            if (u.compare(0, 5, "data:") == 0)
                continue;                       // decoded by cgltf, already capped above
            if (u.find("://") != std::string::npos) {
                message = _L("This glTF file refers to mesh data on the network, which Snapmaker Orca will not fetch.");
                return false;
            }
            if (!uri_is_safe_relative(u)) {
                message = format(_L("This glTF file refers to a data file outside its own folder (%1%)."), u);
                return false;
            }
        }
        const cgltf_result loaded = cgltf_load_buffers(&options, data, path);
        if (loaded != cgltf_result_success) {
            if (file_ctx.too_large)
                message = format(_L("This glTF file refers to more than %1% MB of mesh data, which Snapmaker Orca "
                                    "will not load."),
                                 (unsigned long long) (MAX_GLTF_FILE / (1024 * 1024)));
            else if (loaded == cgltf_result_file_not_found)
                message = _L("A data file this glTF refers to is missing from its folder.");
            else
                message = err_damaged();
            return false;
        }
    }

    // 4. EXT_meshopt_compression, before anything reads an accessor.
    if (!decode_meshopt_buffer_views(data, message))
        return false;

    // cgltf_validate dereferences sparse->indices_buffer_view->buffer without a null check, and a
    // meshopt-compressed view has no outer buffer at all (it lives in the extension). That
    // combination is exotic, but it is a crash, so refuse it rather than risk it.
    for (cgltf_size i = 0; i < data->accessors_count; ++i) {
        const cgltf_accessor &acc = data->accessors[i];
        if (!acc.is_sparse)
            continue;
        if (acc.sparse.indices_buffer_view == nullptr || acc.sparse.indices_buffer_view->buffer == nullptr ||
            acc.sparse.values_buffer_view == nullptr || acc.sparse.values_buffer_view->buffer == nullptr) {
            message = err_damaged();
            return false;
        }
    }

    // 5. cgltf_validate. Its data_too_short family is exactly the "an accessor points outside its
    //    buffer" check - including the sparse-index bound, which cgltf's own unpack does NOT
    //    re-check before writing - so that family is a refusal. The invalid_gltf family is
    //    cosmetic (real files trip it) and is only logged.
    {
        const cgltf_result v = cgltf_validate(data);
        if (v == cgltf_result_data_too_short) {
            message = err_damaged();
            return false;
        }
        if (v != cgltf_result_success)
            BOOST_LOG_TRIVIAL(warning) << "load_gltf: cgltf_validate reported " << (int) v << " for " << path
                                       << "; importing anyway";
    }

    // 6. Pick the scene and enumerate the (node, primitive) pairs it draws.
    const cgltf_scene *scene = data->scene != nullptr ? data->scene
                                                      : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    std::vector<DrawItem>          items;
    std::set<const cgltf_node *>   seen;
    if (scene != nullptr) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            collect_draw_items(scene->nodes[i], 0, seen, items);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (data->nodes[i].parent == nullptr)
                collect_draw_items(&data->nodes[i], 0, seen, items);
    }
    if (items.empty()) {
        message = _L("This glTF file contains no printable geometry.");
        return false;
    }

    // 7. Build the meshes. Nothing touches `model` until every primitive has succeeded, so a
    //    failure leaves the caller's Model exactly as it was.
    const Matrix4d up = up_axis_correction();

    struct BuiltPart
    {
        std::string       name;
        TriangleMesh      mesh;
        Matrix4d          world;
        int               material_index{-1};
        std::vector<RGBA> vertex_colors;
        bool              has_color0{false};
    };
    std::vector<BuiltPart> parts;
    std::set<std::string>  taken_names;

    std::map<const cgltf_material *, int> material_index_of;
    size_t triangles_total       = 0;
    size_t triangle_primitives   = 0;
    size_t out_of_range_indices  = 0;
    size_t empty_after_hygiene   = 0;
    bool   every_part_has_color0 = true;

    const int total_steps = (int) items.size();
    for (size_t step = 0; step < items.size(); ++step) {
        const DrawItem &item = items[step];
        const cgltf_primitive &prim = *item.prim;

        if (progressFn) {
            bool        cancel   = false;
            std::string model_id, code;   // glTF carries neither; leave the Plater's fields empty
            progressFn((int) step, total_steps, cancel, model_id, code);
            if (cancel) {
                message = _L("Import cancelled.");
                return false;
            }
        }

        if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
            prim.type != cgltf_primitive_type_triangle_fan) {
            ++info.dropped_primitives;      // points / lines carry no printable volume
            continue;
        }
        ++triangle_primitives;

        if (prim.has_draco_mesh_compression) {
            // Not in extensionsRequired (an asset may offer a Draco fallback), but cgltf cannot
            // decode it either way and the uncompressed attributes are usually absent.
            message = err_draco();
            return false;
        }

        // The node's world matrix, with the up-axis correction pre-multiplied once.
        // cgltf_node_transform_world walks to the root itself and gets `matrix` vs TRS right.
        float wm[16];
        cgltf_node_transform_world(item.node, wm);
        Matrix4d node_world;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                node_world(r, c) = (double) wm[c * 4 + r];
        const Matrix4d world = up * node_world;

        PrimitiveGeometry geo;
        if (!build_primitive(prim, world, geo, out_of_range_indices, message))
            return false;

        // --- mesh hygiene, in the order the plan fixes ---
        // 1) built above; 2) a mirrored node chain inverts winding; 3) weld (glTF exporters split
        // vertices at every UV/normal seam, so an unwelded cube arrives as 12 loose triangles and
        // lights up the manifold warning); 4) drop degenerates - this one erases faces, so any
        // per-face array must be built after it, not before.
        if (world.block<3, 3>(0, 0).determinant() < 0.0)
            its_flip_triangles(geo.its);
        // Keep the pre-weld colours addressable by position: welding keeps the lowest-index
        // vertex of each exact-coordinate group, so first-seen wins is exactly right.
        std::map<std::array<float, 3>, RGBA> color_at;
        if (geo.has_color0)
            for (size_t i = 0; i < geo.its.vertices.size(); ++i)
                color_at.emplace(std::array<float, 3>{geo.its.vertices[i].x(), geo.its.vertices[i].y(),
                                                      geo.its.vertices[i].z()},
                                 geo.vertex_colors[i]);
        its_merge_vertices(geo.its);
        its_remove_degenerate_faces(geo.its);

        if (geo.its.indices.empty()) {
            ++empty_after_hygiene;
            continue;
        }
        its_compactify_vertices(geo.its);

        triangles_total += geo.its.indices.size();
        if (triangles_total > MAX_GLTF_TRIANGLES) {
            message = format(_L("This glTF file has more than %1% million triangles, which Snapmaker Orca "
                                "will not load."),
                             (unsigned long long) (MAX_GLTF_TRIANGLES / 1000000));
            return false;
        }
        if (parts.size() >= MAX_GLTF_VOLUMES) {
            info.skipped_nodes = items.size() - step;
            message = format(_L("This glTF file has more than %1% parts, which Snapmaker Orca will not load."),
                             (unsigned long long) MAX_GLTF_VOLUMES);
            return false;
        }

        BuiltPart part;
        if (geo.has_color0) {
            part.has_color0 = true;
            part.vertex_colors.reserve(geo.its.vertices.size());
            for (const Vec3f &v : geo.its.vertices) {
                auto it = color_at.find(std::array<float, 3>{v.x(), v.y(), v.z()});
                part.vertex_colors.push_back(it == color_at.end() ? RGBA{1.f, 1.f, 1.f, 1.f} : it->second);
            }
        } else {
            every_part_has_color0 = false;
        }

        // Name: node, else mesh, else "part"; the primitive index only when the mesh has several
        // (a mesh's primitives exist to carry different materials). Then de-duplicate, because
        // two unnamed nodes sharing one mesh would otherwise both be called "part".
        std::string base;
        if (item.node != nullptr && item.node->name != nullptr && *item.node->name != 0)
            base = item.node->name;
        else if (item.node != nullptr && item.node->mesh != nullptr && item.node->mesh->name != nullptr &&
                 *item.node->mesh->name != 0)
            base = item.node->mesh->name;
        else
            base = "part";
        if (item.prim_count > 1)
            base += "_" + std::to_string(item.prim_index + 1);
        part.name = unique_part_name(base, taken_names);

        // Materials: one sRGB colour per distinct material actually drawn. Stage 2 maps these to
        // filament slots; Stage 1 only records them.
        if (prim.material != nullptr) {
            auto found = material_index_of.find(prim.material);
            if (found != material_index_of.end()) {
                part.material_index = found->second;
            } else {
                RGBA c{1.f, 1.f, 1.f, 1.f};
                if (prim.material->has_pbr_metallic_roughness) {
                    const cgltf_float *f = prim.material->pbr_metallic_roughness.base_color_factor;
                    c = RGBA{linear_to_srgb((float) f[0]), linear_to_srgb((float) f[1]),
                             linear_to_srgb((float) f[2]), (float) f[3]};
                }
                int index = -1;
                for (size_t m = 0; m < info.material_colors.size(); ++m) {
                    const RGBA &e = info.material_colors[m];
                    if (std::fabs(e[0] - c[0]) < 1.f / 255.f && std::fabs(e[1] - c[1]) < 1.f / 255.f &&
                        std::fabs(e[2] - c[2]) < 1.f / 255.f && std::fabs(e[3] - c[3]) < 1.f / 255.f) {
                        index = (int) m;
                        break;
                    }
                }
                if (index < 0) {
                    info.material_colors.push_back(c);
                    index = (int) info.material_colors.size() - 1;
                }
                material_index_of.emplace(prim.material, index);
                part.material_index = index;
            }
            if (prim.material->has_pbr_metallic_roughness &&
                prim.material->pbr_metallic_roughness.base_color_texture.texture != nullptr)
                info.had_textures = true;
        }

        part.world = world;
        // Note: TriangleMesh(indexed_triangle_set&&) only fills stats, it does not run admesh
        // repair - repair is reachable only through TriangleMesh::from_stl and is STL-only today.
        // glTF follows the OBJ precedent: the object list's manifold warning stays the remedy.
        part.mesh = TriangleMesh(std::move(geo.its));
        parts.push_back(std::move(part));
    }

    if (progressFn) {
        bool        cancel = false;
        std::string model_id, code;
        progressFn(total_steps, total_steps, cancel, model_id, code);
    }

    if (parts.empty()) {
        if (triangle_primitives == 0 && info.dropped_primitives > 0)
            message = _L("This file contains no printable surfaces (only points or lines).");
        else
            message = _L("This glTF file contains no printable geometry.");
        return false;
    }

    if (out_of_range_indices > 0)
        BOOST_LOG_TRIVIAL(warning) << "load_gltf: " << out_of_range_indices
                                   << " triangle(s) referenced vertices outside their primitive and were dropped";
    if (empty_after_hygiene > 0)
        BOOST_LOG_TRIVIAL(info) << "load_gltf: " << empty_after_hygiene << " primitive(s) had no non-degenerate faces";
    if (info.dropped_primitives > 0)
        BOOST_LOG_TRIVIAL(info) << "load_gltf: dropped " << info.dropped_primitives << " point/line primitive(s)";

    // 8. Build the one ModelObject.
    std::string object_name;
    if (object_name_in != nullptr && *object_name_in != 0)
        object_name = object_name_in;
    else if (scene != nullptr && scene->name != nullptr && *scene->name != 0)
        object_name = scene->name;
    else
        object_name = file_stem_utf8(path);

    ModelObject *object = model->add_object();
    object->name        = object_name;
    object->input_file  = path;

    const int object_idx = (int) model->objects.size() - 1;
    info.parts.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        BuiltPart &part = parts[i];
        // add_volume centres the mesh on its own bbox and puts the shift into the volume offset,
        // so world position survives and the volume keeps an identity rotation and unit scale.
        ModelVolume *volume = object->add_volume(std::move(part.mesh), ModelVolumeType::MODEL_PART);
        volume->name        = part.name;
        volume->source.input_file = path;
        volume->source.object_idx = object_idx;
        volume->source.volume_idx = (int) i;
        // The matrix we baked, kept so a future "reload from disk" can reproduce the placement.
        Transform3d t = Transform3d::Identity();
        t.matrix()    = part.world;
        volume->source.transform = Geometry::Transformation(t);

        GltfPart out_part;
        out_part.name           = part.name;
        out_part.material_index = part.material_index;
        out_part.triangles      = volume->mesh().its.indices.size();
        info.parts.push_back(std::move(out_part));

        if (every_part_has_color0)
            info.vertex_colors.insert(info.vertex_colors.end(), part.vertex_colors.begin(), part.vertex_colors.end());
    }
    if (!every_part_has_color0)
        info.vertex_colors.clear();
    info.is_single_material = info.material_colors.size() <= 1;

    // BBS: mirror Model::add_object - a fresh object needs a valid extruder id.
    if (!object->config.has("extruder") || object->config.extruder() == 0)
        object->config.set_key_value("extruder", new ConfigOptionInt(0));
    object->invalidate_bounding_box();

    if (info.had_textures)
        message = _L("This model's colours come from a texture, which was not imported.");

    BOOST_LOG_TRIVIAL(info) << "load_gltf: " << path << " -> 1 object, " << info.parts.size() << " part(s), "
                            << triangles_total << " triangle(s), " << info.material_colors.size() << " material(s)";
    return true;
}

} // namespace Slic3r
