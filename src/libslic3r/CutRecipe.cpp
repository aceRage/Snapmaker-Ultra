#include "CutRecipe.hpp"

#include "Model.hpp"
#include "ImageFill.hpp"   // image_fill_sha256_hex - the project's SHA-256 helper

#include <cstring>
#include <cmath>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Equality. Exact for the discrete fields; the floating-point ones are compared
// bit-for-bit on purpose. This is a ROUND-TRIP test predicate, not a geometric
// one: the serializer writes doubles at full precision, so a value that comes
// back changed at all means the serialization lost something.
// ---------------------------------------------------------------------------

bool CutRecipeGroove::operator==(const CutRecipeGroove& o) const
{
    return depth == o.depth && width == o.width && flaps_angle == o.flaps_angle && angle == o.angle &&
           depth_init == o.depth_init && width_init == o.width_init &&
           flaps_angle_init == o.flaps_angle_init && angle_init == o.angle_init &&
           depth_tolerance == o.depth_tolerance && width_tolerance == o.width_tolerance;
}

bool CutRecipeConnector::operator==(const CutRecipeConnector& o) const
{
    return pos == o.pos && rotation_m.matrix() == o.rotation_m.matrix() &&
           radius == o.radius && height == o.height &&
           radius_tolerance == o.radius_tolerance && height_tolerance == o.height_tolerance &&
           z_angle == o.z_angle && type == o.type && style == o.style && shape == o.shape &&
           flexi == o.flexi;
}

bool CutRecipeStroke::operator==(const CutRecipeStroke& o) const
{
    if (closed != o.closed || smoothing != o.smoothing || samples.size() != o.samples.size())
        return false;
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i].pos != o.samples[i].pos || samples[i].normal != o.samples[i].normal ||
            samples[i].facet != o.samples[i].facet)
            return false;
    return true;
}

bool CutRecipeSheet::operator==(const CutRecipeSheet& o) const
{
    return nx == o.nx && ny == o.ny && half_size_u == o.half_size_u && half_size_v == o.half_size_v &&
           values == o.values;
}

bool CutRecipe::operator==(const CutRecipe& o) const
{
    if (version != o.version || kind != o.kind)
        return false;
    if (plane_center != o.plane_center || rotation_m.matrix() != o.rotation_m.matrix())
        return false;
    if (sheet != o.sheet || stroke != o.stroke || groove != o.groove)
        return false;
    if (draw_direction != o.draw_direction || draw_view_dir != o.draw_view_dir ||
        draw_extension != o.draw_extension || draw_angle_deg != o.draw_angle_deg ||
        draw_through_all != o.draw_through_all || draw_depth != o.draw_depth)
        return false;
    if (thickness != o.thickness || thickness_offset != o.thickness_offset)
        return false;
    if (keep_upper != o.keep_upper || keep_lower != o.keep_lower || keep_as_parts != o.keep_as_parts ||
        place_on_cut_upper != o.place_on_cut_upper || place_on_cut_lower != o.place_on_cut_lower ||
        rotate_upper != o.rotate_upper || rotate_lower != o.rotate_lower)
        return false;
    if (upper_visibility != o.upper_visibility || lower_visibility != o.lower_visibility)
        return false;
    if (connectors != o.connectors)
        return false;
    // The mesh is compared by its content hash, which is what the 3MF stores and
    // what makes two halves share one blob.
    return mesh_hash == o.mesh_hash;
}

bool CutRecipe::valid() const
{
    if (version != CutRecipeVersion)
        return false;
    if (!has_mesh())
        return false;
    switch (kind) {
    case CutRecipeKind::Curved: return sheet.valid();
    // A stroke needs enough raw samples that finish() can produce a path at all.
    case CutRecipeKind::Drawn:  return stroke.samples.size() >= size_t(DrawCutStroke::MinSamples);
    case CutRecipeKind::Plane:
    case CutRecipeKind::Groove: return true;
    }
    return false;
}

DrawCutParams CutRecipe::draw_params() const
{
    DrawCutParams p;
    p.direction        = (draw_direction >= int(DrawCutDirection::SurfaceNormal) &&
                          draw_direction <= int(DrawCutDirection::AxisZ)) ?
                         DrawCutDirection(draw_direction) : DrawCutDirection::SurfaceNormal;
    p.view_dir         = draw_view_dir;
    p.extension        = draw_extension;
    p.angle_deg        = draw_angle_deg;
    p.through_all      = draw_through_all;
    p.depth            = draw_depth;
    p.thickness        = thickness;
    p.thickness_offset = thickness_offset;
    return p;
}

CurvedCutSheet CutRecipe::curved_sheet() const
{
    CurvedCutSheet s;
    if (!sheet.valid())
        return s;
    s.reset(sheet.nx, sheet.ny);
    s.set_half_size(sheet.half_size_u, sheet.half_size_v);
    s.set_values(sheet.values);
    return s;
}

// ---------------------------------------------------------------------------
// Connector conversion.
// ---------------------------------------------------------------------------

void cut_recipe_connectors_from_model(const std::vector<CutConnector>& in, std::vector<CutRecipeConnector>& out)
{
    out.clear();
    out.reserve(in.size());
    for (const CutConnector& c : in) {
        CutRecipeConnector r;
        r.pos              = c.pos;
        r.rotation_m       = c.rotation_m;
        r.radius           = c.radius;
        r.height           = c.height;
        r.radius_tolerance = c.radius_tolerance;
        r.height_tolerance = c.height_tolerance;
        r.z_angle          = c.z_angle;
        r.type             = int(c.attribs.type);
        r.style            = int(c.attribs.style);
        r.shape            = int(c.attribs.shape);
        r.flexi            = c.flexi;
        out.emplace_back(r);
    }
}

void cut_recipe_connectors_to_model(const std::vector<CutRecipeConnector>& in, std::vector<CutConnector>& out)
{
    out.clear();
    out.reserve(in.size());
    for (const CutRecipeConnector& r : in) {
        // The recipe may have come from a file: clamp every enum before casting,
        // the same discipline the cut_information.xml reader follows.
        const int type  = (r.type  >= 0 && r.type  <= int(CutConnectorType::FlexiJoint)) ? r.type  : int(CutConnectorType::Plug);
        const int style = (r.style >= 0 && r.style <= int(CutConnectorStyle::Undef))     ? r.style : int(CutConnectorStyle::Prism);
        const int shape = (r.shape >= 0 && r.shape <= int(CutConnectorShape::Undef))     ? r.shape : int(CutConnectorShape::Circle);
        CutConnector c(r.pos, r.rotation_m, r.radius, r.height, r.radius_tolerance, r.height_tolerance, r.z_angle,
                       CutConnectorAttributes(CutConnectorType(type), CutConnectorStyle(style), CutConnectorShape(shape)));
        c.flexi = r.flexi;
        out.emplace_back(c);
    }
}

// ---------------------------------------------------------------------------
// The mesh blob.
//
// Deliberately NOT STL. An STL round trip would lose the vertex indexing (every
// facet carries its own three vertices) and would re-weld on the way back, so
// the mesh that came out would not be the mesh that went in - and the whole
// point of storing it is that re-cutting reproduces the SAME halves. This keeps
// the indexed triangle set exactly as it stands.
// ---------------------------------------------------------------------------

static const char   CutRecipeMeshMagic[8] = { 'E', 'S', 'C', 'U', 'T', 'M', 'S', 'H' };
static const uint32_t CutRecipeMeshBlobVersion = 1;

// A blob bigger than this is not a mesh we wrote; refuse it rather than trying
// to allocate whatever a corrupt header claims. 200M triangles is far past any
// printable model and still far short of overflowing the arithmetic below.
static const uint64_t CutRecipeMeshMaxFacets   = 200ull * 1000ull * 1000ull;
static const uint64_t CutRecipeMeshMaxVertices = 200ull * 1000ull * 1000ull;

static void blob_put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(uint8_t( x        & 0xFF));
    v.push_back(uint8_t((x >>  8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 24) & 0xFF));
}

static uint32_t blob_get_u32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void blob_put_f32(std::vector<uint8_t>& v, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    blob_put_u32(v, bits);
}

static float blob_get_f32(const uint8_t* p)
{
    const uint32_t bits = blob_get_u32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::vector<uint8_t> cut_recipe_mesh_to_blob(const TriangleMesh& mesh)
{
    const indexed_triangle_set& its = mesh.its;
    std::vector<uint8_t> blob;
    if (its.vertices.size() > CutRecipeMeshMaxVertices || its.indices.size() > CutRecipeMeshMaxFacets)
        return blob;
    blob.reserve(16 + its.vertices.size() * 12 + its.indices.size() * 12);
    blob.insert(blob.end(), CutRecipeMeshMagic, CutRecipeMeshMagic + sizeof(CutRecipeMeshMagic));
    blob_put_u32(blob, CutRecipeMeshBlobVersion);
    blob_put_u32(blob, uint32_t(its.vertices.size()));
    blob_put_u32(blob, uint32_t(its.indices.size()));
    for (const Vec3f& v : its.vertices) {
        blob_put_f32(blob, v.x());
        blob_put_f32(blob, v.y());
        blob_put_f32(blob, v.z());
    }
    for (const Vec3i32& f : its.indices) {
        blob_put_u32(blob, uint32_t(f[0]));
        blob_put_u32(blob, uint32_t(f[1]));
        blob_put_u32(blob, uint32_t(f[2]));
    }
    return blob;
}

bool cut_recipe_mesh_from_blob(const std::vector<uint8_t>& blob, TriangleMesh& out)
{
    out = TriangleMesh();
    // Untrusted file content throughout: every length is checked against what is
    // actually there before a single byte is read past the header.
    if (blob.size() < 20)
        return false;
    if (std::memcmp(blob.data(), CutRecipeMeshMagic, sizeof(CutRecipeMeshMagic)) != 0)
        return false;
    const uint32_t version = blob_get_u32(blob.data() + 8);
    if (version != CutRecipeMeshBlobVersion)
        return false;
    const uint64_t n_vert  = blob_get_u32(blob.data() + 12);
    const uint64_t n_facet = blob_get_u32(blob.data() + 16);
    if (n_vert > CutRecipeMeshMaxVertices || n_facet > CutRecipeMeshMaxFacets)
        return false;
    const uint64_t need = 20ull + n_vert * 12ull + n_facet * 12ull;
    if (uint64_t(blob.size()) != need)
        return false;

    indexed_triangle_set its;
    its.vertices.resize(size_t(n_vert));
    its.indices.resize(size_t(n_facet));
    const uint8_t* p = blob.data() + 20;
    for (size_t i = 0; i < size_t(n_vert); ++i, p += 12)
        its.vertices[i] = Vec3f(blob_get_f32(p), blob_get_f32(p + 4), blob_get_f32(p + 8));
    for (size_t i = 0; i < size_t(n_facet); ++i, p += 12) {
        const uint32_t a = blob_get_u32(p), b = blob_get_u32(p + 4), c = blob_get_u32(p + 8);
        // An index past the vertex array would be read out of bounds by every
        // consumer of the mesh downstream.
        if (a >= n_vert || b >= n_vert || c >= n_vert)
            return false;
        its.indices[i] = Vec3i32(int(a), int(b), int(c));
    }
    out = TriangleMesh(std::move(its));
    return true;
}

std::string cut_recipe_mesh_hash(const std::vector<uint8_t>& blob)
{
    return image_fill_sha256_hex(blob);
}

} // namespace Slic3r
