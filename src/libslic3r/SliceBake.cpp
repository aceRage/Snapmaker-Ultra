#include "SliceBake.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "SlicesToTriangleMesh.hpp"
#include "TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {

namespace {

// The roles that make up the OUTER wall.
//
// erExternalPerimeter is the outer wall proper. The other two are the same physical loop split by
// role where it leaves the layer below: erOverhangPerimeter where it hangs over air, and this
// fork's erOverSupportPerimeter where it lands on support material. Both are outer wall and both
// must be baked - dropping them punches holes in exactly the overhanging bands where the surface
// matters most. (erPerimeter, the inner walls, is deliberately never read: the bake is solid, so
// everything inward of the outer loop is filled by construction.)
bool is_outer_wall(ExtrusionRole role)
{
    return role == erExternalPerimeter || role == erOverhangPerimeter || role == erOverSupportPerimeter;
}

// One outer-wall loop's printed footprint, appended to `out`.
//
// The stored polyline is the extrusion's CENTRELINE; what the nozzle lays down is that centreline
// swept by the path's own `width`. The printed outer boundary is therefore the centreline offset
// outward by width/2.
//
// The loop is offset as a CLOSED POLYGON, not as a set of open polylines, and this is the trick
// that makes the bake solid. Offsetting the open polylines (what
// ExtrusionPath::polygons_covered_by_width does) sweeps a capsule along each path, and the union
// of those capsules around a closed ring is an ANNULUS - a ring with the object's own cross
// section punched out as a hole. Offsetting the closed polygon instead gives the filled region
// bounded by the outer edge of the wall, which is exactly the printed cross section.
//
// Orientation carries the meaning here, and the union below reads it. A CONTOUR loop runs
// counter-clockwise and its printed region is everything inside it out to width/2 BEYOND the
// centreline: offset the CCW polygon by +width/2. A HOLE loop runs clockwise and the material is
// everything OUTSIDE it, so the void it encloses reaches width/2 INSIDE the centreline: offsetting
// the CW polygon by the same +width/2 grows it along its own (reversed) normal, which is exactly
// the shrink the hole needs, and it stays clockwise. Handing both to a pftNonZero union then makes
// the contours solid and the holes hollow, with no separate bookkeeping.
//
// The width is the loop's own, taken as the MAXIMUM over its outer-wall paths. A loop crossing an
// overhang is split into paths of differing width (overhang flow is not wall flow), and a closed
// offset takes one delta; the larger is the safe choice, since it can only push the boundary out
// by at most the difference - well under a tenth of a millimetre - whereas the smaller would pull
// the printed edge inside where the wide half actually lands.
void append_loop_footprint(const ExtrusionLoop &loop, Polygons &out, size_t &loops_used)
{
    float width = 0.f;
    bool  any   = false;
    for (const ExtrusionPath &path : loop.paths) {
        // A loop's own role() is erMixed once it has been split by overhang, so the per-path role
        // is the only reliable one.
        if (is_outer_wall(path.role())) {
            any   = true;
            width = std::max(width, path.width);
        }
    }
    if (! any || width <= 0.f)
        return;

    // The WHOLE ring, not just its outer-wall paths: a loop is one closed curve, and dropping an
    // overhanging section would leave a polygon that does not close. The role test above decides
    // whether the loop is an outer wall, not which parts of it exist.
    Polygon poly = loop.polygon();
    if (poly.points.size() < 3)
        return;

    // The tiny extra ClipperSafetyOffset makes the footprints of neighbouring loops (two islands
    // that touch) overlap rather than meet exactly, so the union closes the joint instead of
    // leaving a zero-width crack there.
    const float delta = float(scale_(width / 2.)) + ClipperSafetyOffset;
    Polygons    grown = offset(poly, delta);
    // offset() normalises what it returns to CCW outer / CW hole, so a hole loop's grown ring
    // comes back as a positive contour. Put its orientation back, so the union reads it as the
    // void it is.
    if (poly.is_clockwise())
        for (Polygon &p : grown)
            p.make_clockwise();
    polygons_append(out, std::move(grown));
    ++loops_used;
}

void collect_outer_walls(const ExtrusionEntity *entity, Polygons &out, size_t &loops_used)
{
    if (entity == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_outer_walls(child, out, loops_used);
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        append_loop_footprint(*loop, out, loops_used);
        return;
    }
    // ExtrusionMultiPath before ExtrusionPath: the two are siblings, not parent and child, but
    // testing the leaf first would still be wrong the day one of them gains a common base.
    //
    // Neither is a closed ring, so neither can be offset as a polygon. An outer wall that reaches
    // the bake as an open path is a rarity (a wall too short to close, chiefly), and the capsule
    // around it is the right footprint for it: an open stroke encloses no interior to fill.
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            if (is_outer_wall(path.role()) && path.polyline.points.size() >= 2 && path.width > 0.f) {
                path.polygons_covered_by_width(out, float(ClipperSafetyOffset));
                ++loops_used;
            }
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        if (is_outer_wall(path->role()) && path->polyline.points.size() >= 2 && path->width > 0.f) {
            path->polygons_covered_by_width(out, float(ClipperSafetyOffset));
            ++loops_used;
        }
    }
}

bool layer_has_outer_wall(const Layer &layer)
{
    Polygons scratch;
    size_t   n = 0;
    for (const LayerRegion *region : layer.regions()) {
        for (const ExtrusionEntity *entity : region->perimeters.entities) {
            collect_outer_walls(entity, scratch, n);
            if (n > 0)
                return true;
        }
    }
    return false;
}

void clamp_range(const PrintObject &object, const SliceBakeOptions &opts, size_t &begin, size_t &end)
{
    const size_t n = object.layer_count();
    begin = std::min(opts.layer_begin, n);
    end   = std::min(opts.layer_end, n);
    if (end < begin)
        end = begin;
}

} // namespace

bool slice_bake_available(const PrintObject &object)
{
    for (const Layer *layer : object.layers())
        if (layer != nullptr && layer_has_outer_wall(*layer))
            return true;
    return false;
}

size_t slice_bake_estimate_triangles(const PrintObject &object, const SliceBakeOptions &opts)
{
    size_t begin = 0, end = 0;
    clamp_range(object, opts, begin, end);

    size_t points = 0;
    for (size_t i = begin; i < end; ++i) {
        const Layer *layer = object.get_layer(int(i));
        if (layer == nullptr)
            continue;
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->perimeters.entities) {
                Polylines pls;
                entity->collect_polylines(pls);
                for (const Polyline &pl : pls)
                    points += pl.points.size();
            }
    }
    // Two wall-strip triangles per boundary point, and the caps add roughly as many again over
    // the whole part on a shape whose footprint changes layer to layer. Deliberately a plain
    // doubling: a figure shown as "about N" must not pretend to know the tesselation.
    return points * 2;
}

std::vector<ExPolygons> slice_bake_layer_regions(const PrintObject       &object,
                                                 const SliceBakeOptions  &opts,
                                                 std::vector<double>     *out_z,
                                                 std::vector<double>     *out_bottom_z,
                                                 SliceBakeReport         *report,
                                                 const SliceBakeProgress &progress)
{
    size_t begin = 0, end = 0;
    clamp_range(object, opts, begin, end);

    std::vector<ExPolygons> slices;
    slices.reserve(end - begin);
    if (out_z != nullptr)        out_z->clear();
    if (out_bottom_z != nullptr) out_bottom_z->clear();

    const double close_radius = std::clamp(opts.close_gaps_radius, 0., SLICE_BAKE_CLOSE_GAPS_MAX);
    size_t loops_used = 0, empty_layers = 0;

    // Sequential on purpose. The per-layer work is one Clipper union of a few hundred polygons -
    // small next to the loft that follows - and a serial loop is the cheapest way to guarantee
    // the output ordering is a pure function of the input, which is what "deterministic" has to
    // mean here.
    for (size_t i = begin; i < end; ++i) {
        if (progress) {
            // The footprints are the first half of the bake; the loft is the second.
            const int pct = end > begin ? int(50. * double(i - begin) / double(end - begin)) : 0;
            if (! progress(pct))
                throw SliceBakeCancelled();
        }

        const Layer *layer = object.get_layer(int(i));
        if (layer == nullptr) {
            ++empty_layers;
            continue;
        }

        Polygons footprint;
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->perimeters.entities)
                collect_outer_walls(entity, footprint, loops_used);

        if (footprint.empty()) {
            // A layer with no outer wall contributes nothing and is dropped rather than lofted as
            // an empty slice - an empty ExPolygons in the middle of the stack would pinch the
            // mesh shut and reopen it, which is not what the object looks like.
            ++empty_layers;
            continue;
        }

        // Union with pftNonZero fills every loop's interior: the offset ring of a closed loop
        // encloses its own inside, and the non-zero rule keeps that inside solid instead of
        // treating it as a hole. This is what makes the bake a SOLID - and what makes top and
        // bottom surfaces implicit, since a filled top layer already is the top skin.
        ExPolygons filled = union_ex(footprint, ClipperLib::pftNonZero);

        if (close_radius > 0.) {
            const float delta = float(scale_(close_radius));
            filled = closing_ex(to_polygons(filled), delta, delta);
        }

        if (filled.empty()) {
            ++empty_layers;
            continue;
        }

        slices.emplace_back(std::move(filled));
        if (out_z != nullptr)        out_z->push_back(layer->print_z);
        if (out_bottom_z != nullptr) out_bottom_z->push_back(layer->bottom_z());
    }

    if (report != nullptr) {
        report->layers_baked = slices.size();
        report->loops_used   = loops_used;
        report->empty_layers = empty_layers;
    }
    return slices;
}

indexed_triangle_set slice_bake_to_mesh(const PrintObject       &object,
                                        const SliceBakeOptions  &opts,
                                        SliceBakeReport         *report,
                                        const SliceBakeProgress &progress)
{
    SliceBakeReport local;
    SliceBakeReport &rep = report != nullptr ? *report : local;

    std::vector<double> z, bottom_z;
    std::vector<ExPolygons> slices = slice_bake_layer_regions(object, opts, &z, &bottom_z, &rep, progress);

    if (slices.empty()) {
        rep.note = "the object has no outer wall to bake - slice the plate first";
        return {};
    }

    if (progress && ! progress(55))
        throw SliceBakeCancelled();

    // The loft assigns slice 0 the band [zmin, grid[0]] and slice i>0 the band
    // [grid[i-1], grid[i]], so grid[i] is the TOP of slice i. A printed layer occupies
    // [bottom_z, print_z], which makes grid[i] = the layer's own print_z and zmin = the first
    // baked layer's bottom_z an exact reproduction of the printed stack.
    //
    // The real print_z values are used rather than the constant-height convenience overload
    // because they are right in the two cases that overload gets wrong: a variable or adaptive
    // layer height, and a layer subset that does not start at the bed.
    const double zmin = bottom_z.front();
    std::vector<float> grid;
    grid.reserve(z.size());
    for (double v : z)
        grid.push_back(float(v));

    indexed_triangle_set mesh = slices_to_mesh(slices, zmin, grid);

    if (progress && ! progress(85))
        throw SliceBakeCancelled();

    if (opts.in_object_frame) {
        // The sliced geometry lives in PRINT space: the object's mesh has been through
        // trafo_centered() (its instance transform, then a translation that centres it in XY for
        // slicing). Undo exactly that, so the baked mesh lands where the ModelVolume's own mesh
        // sits and can replace it without the object jumping.
        const Transform3d inv = object.trafo_centered().inverse();
        for (Vec3f &v : mesh.vertices)
            v = (inv * v.cast<double>()).cast<float>();
        // A mirroring or negative-determinant instance transform flips the winding; put it back
        // so the replacement mesh is not inside-out.
        if (inv.linear().determinant() < 0.)
            for (Vec3i32 &f : mesh.indices)
                std::swap(f(1), f(2));
    }

    rep.triangles  = mesh.indices.size();
    rep.vertices   = mesh.vertices.size();
    rep.watertight = ! mesh.indices.empty() && its_num_open_edges(mesh) == 0;
    if (! mesh.vertices.empty()) {
        rep.z_min = rep.z_max = mesh.vertices.front().z();
        for (const Vec3f &v : mesh.vertices) {
            rep.z_min = std::min(rep.z_min, double(v.z()));
            rep.z_max = std::max(rep.z_max, double(v.z()));
        }
    }

    BOOST_LOG_TRIVIAL(info) << "slice_bake: " << rep.layers_baked << " layers, " << rep.loops_used
                            << " outer-wall paths, " << rep.triangles << " triangles, "
                            << (rep.watertight ? "watertight" : "NOT watertight")
                            << ", " << rep.empty_layers << " empty layer(s) skipped";

    if (progress && ! progress(100))
        throw SliceBakeCancelled();

    return mesh;
}

} // namespace Slic3r
