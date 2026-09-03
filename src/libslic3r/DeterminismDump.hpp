#ifndef slic3r_DeterminismDump_hpp_
#define slic3r_DeterminismDump_hpp_

// A textual, byte-stable dump of a PrintObject's per-layer geometry, written only when the
// environment variable ORCA_DET_DUMP names a directory. It exists to answer one question when two
// runs of the same binary produce different G-code: at which pipeline stage do they first diverge?
// Slice the same project twice with ORCA_DET_DUMP pointing at two different directories and diff
// the files; the first stage whose file differs is the stage that introduced the difference.
//
// Nothing here runs unless the variable is set: the getenv is read once into a static.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Layer.hpp"
#include "Print.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"

namespace Slic3r {

inline const std::string &det_dump_dir()
{
    static const std::string dir = []() -> std::string {
        const char *v = std::getenv("ORCA_DET_DUMP");
        return v ? std::string(v) : std::string();
    }();
    return dir;
}

inline bool det_dump_enabled() { return !det_dump_dir().empty(); }

inline size_t det_dump_object_index(const PrintObject &object)
{
    const auto &objs = object.print()->objects();
    for (size_t i = 0; i < objs.size(); ++i)
        if (objs[i] == &object)
            return i;
    return objs.size();
}

inline void det_dump_append(std::string &out, const Points &pts)
{
    for (const Point &p : pts) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %lld,%lld", (long long) p.x(), (long long) p.y());
        out += buf;
    }
}

inline void det_dump_append(std::string &out, const ExPolygon &ex)
{
    out += "  contour";
    det_dump_append(out, ex.contour.points);
    out += "\n";
    for (const Polygon &h : ex.holes) {
        out += "  hole";
        det_dump_append(out, h.points);
        out += "\n";
    }
}

// Write (or append) one free-form block, one file per layer, so the parallel per-layer fill
// generation can dump without any cross-thread sharing.
inline void det_dump_text_mode(const char *name, int layer_id, const std::string &body, const char *mode)
{
    if (!det_dump_enabled())
        return;
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "_L%04d.txt", layer_id);
    const std::string path = det_dump_dir() + "/" + name + suffix;
    if (FILE *f = std::fopen(path.c_str(), mode)) {
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
    }
}

inline void det_dump_text(const char *name, int layer_id, const std::string &body)
{
    det_dump_text_mode(name, layer_id, body, "wb");
}

inline void det_dump_text_append(const char *name, int layer_id, const std::string &body)
{
    det_dump_text_mode(name, layer_id, body, "ab");
}

// Dump the slices and fill surfaces of every layer region, in layer / region order.
inline void det_dump_surfaces(const PrintObject &object, const char *stage)
{
    if (!det_dump_enabled())
        return;
    std::string out;
    out.reserve(1 << 20);
    for (size_t li = 0; li < object.layer_count(); ++li) {
        const Layer *layer = object.get_layer(int(li));
        char hdr[128];
        std::snprintf(hdr, sizeof(hdr), "layer %zu print_z %.6f slice_z %.6f\n", li, layer->print_z, layer->slice_z);
        out += hdr;
        for (size_t ri = 0; ri < layer->regions().size(); ++ri) {
            const LayerRegion *lr = layer->regions()[ri];
            std::snprintf(hdr, sizeof(hdr), " region %zu slices %zu fill_surfaces %zu\n", ri,
                          lr->slices.surfaces.size(), lr->fill_surfaces.surfaces.size());
            out += hdr;
            for (const Surface &s : lr->slices.surfaces) {
                std::snprintf(hdr, sizeof(hdr), "  slice type %d\n", int(s.surface_type));
                out += hdr;
                det_dump_append(out, s.expolygon);
            }
            for (const Surface &s : lr->fill_surfaces.surfaces) {
                std::snprintf(hdr, sizeof(hdr), "  fill type %d bridge_angle %.9f\n", int(s.surface_type), s.bridge_angle);
                out += hdr;
                det_dump_append(out, s.expolygon);
            }
        }
    }
    const std::string path = det_dump_dir() + "/obj" + std::to_string(det_dump_object_index(object)) + "_" + stage + ".txt";
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

inline void det_dump_entity(std::string &out, const ExtrusionEntity *e, int depth)
{
    char hdr[160];
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        std::snprintf(hdr, sizeof(hdr), "%*scollection n=%zu no_sort=%d\n", depth * 2, "", coll->entities.size(),
                      int(coll->no_sort));
        out += hdr;
        for (const ExtrusionEntity *c : coll->entities)
            det_dump_entity(out, c, depth + 1);
        return;
    }
    Polylines pls;
    e->collect_polylines(pls);
    std::snprintf(hdr, sizeof(hdr), "%*sentity role=%d len=%.6f\n", depth * 2, "", int(e->role()),
                  e->length() * SCALING_FACTOR);
    out += hdr;
    for (const Polyline &pl : pls) {
        out += std::string(depth * 2 + 2, ' ') + "pl";
        det_dump_append(out, pl.points);
        out += "\n";
    }
}

// Dump the perimeter / fill extrusions of every layer region, in layer / region order.
inline void det_dump_extrusions(const PrintObject &object, const char *stage)
{
    if (!det_dump_enabled())
        return;
    std::string out;
    out.reserve(1 << 20);
    for (size_t li = 0; li < object.layer_count(); ++li) {
        const Layer *layer = object.get_layer(int(li));
        char hdr[128];
        std::snprintf(hdr, sizeof(hdr), "layer %zu print_z %.6f\n", li, layer->print_z);
        out += hdr;
        for (size_t ri = 0; ri < layer->regions().size(); ++ri) {
            const LayerRegion *lr = layer->regions()[ri];
            std::snprintf(hdr, sizeof(hdr), " region %zu\n", ri);
            out += hdr;
            out += "  perimeters\n";
            det_dump_entity(out, &lr->perimeters, 2);
            out += "  fills\n";
            det_dump_entity(out, &lr->fills, 2);
        }
    }
    const std::string path = det_dump_dir() + "/obj" + std::to_string(det_dump_object_index(object)) + "_" + stage + ".txt";
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

} // namespace Slic3r

#endif // slic3r_DeterminismDump_hpp_
