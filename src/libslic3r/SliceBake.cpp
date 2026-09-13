#include "SliceBake.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "SlicesToTriangleMesh.hpp"
#include "Tesselate.hpp"
#include "TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

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

// ---------------------------------------------------------------------------------------------
// The loft: one closed prism per layer
// ---------------------------------------------------------------------------------------------
//
// Why not Slic3r::slices_to_mesh, which this bake used first: that loft triangulates the caps
// between consecutive layers from CLIPPER DIFFS (diff_ex(lower, upper) and diff_ex(upper, lower))
// while the vertical walls come from the layer polygons themselves. A diff introduces
// intersection vertices on edges the wall strip has no vertex on, so the two meshes meet along
// edges split on one side and whole on the other - T-junctions, which its_merge_vertices cannot
// weld because the vertices genuinely are not coincident. Its own FIXME says as much
// ("there will be cracks in the output"). A prismatic cube never differs layer to layer and so
// came out clean; a fuzzed cube produced 20,264 open edges and a cylinder 15,245.
//
// What is built instead: each layer is its own CLOSED prism - bottom cap, vertical walls, top cap
// - and every one of the three is generated from the SAME contour point set, so the prism is
// watertight on its own, by construction, with no diff and no tolerance anywhere. Stacking the
// prisms and welding exactly-coincident vertices then gives a mesh whose only remaining internal
// structure is a pair of coplanar, oppositely-wound cap faces between neighbouring layers. Those
// pairs are matched (each is somebody's top and somebody's bottom), so every edge still has an
// even number of incident faces and the result is closed: its_num_open_edges() == 0.
//
// The cost is the coplanar internal faces where two layers overlap. They are the price of never
// needing a diff, and nothing downstream minds: a closed mesh is a closed mesh to TriangleMesh's
// statistics, to the repair path, and to the slicer, which cuts the same contours either way.
//
// The cap triangulation is the existing GLU tesselator. It emits its vertices verbatim from the
// coordinates handed in - the same unscale<double>() the wall vertices use - so a cap vertex can
// be matched back to its contour point by exact double equality. The one case that would not
// match is a tessCombine vertex, which GLU only produces for a self-intersecting contour; the
// union_ex output here is not self-intersecting, and if one ever appeared it is counted into
// report.note rather than silently welded to the wrong place.

// Exact-match table from an unscaled XY (as the tesselator emits it) to the pair of mesh vertex
// indices for that point at the band's bottom and top Z.
struct PrismVertices
{
    std::map<std::pair<double, double>, std::pair<int, int>> index;
    size_t unmatched = 0;
};

// Append the two vertices (lo, hi) for every point of `poly` and record them in `pv`.
// Returns the index of the first LO vertex; the walls are emitted over that contiguous run.
int append_ring_vertices(indexed_triangle_set &mesh, PrismVertices &pv, const Polygon &poly,
                         double z_lo, double z_hi)
{
    const int first_lo = int(mesh.vertices.size());
    const int n        = int(poly.points.size());
    // Both runs are contiguous: lo vertices [first_lo, first_lo+n), hi [first_lo+n, +2n).
    for (const Point &p : poly.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_lo)));
    for (const Point &p : poly.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_hi)));

    for (int i = 0; i < n; ++i) {
        const Point &p = poly.points[size_t(i)];
        // unscale<double> is exactly what the tesselator will apply to the same Point.
        pv.index.emplace(std::make_pair(unscale<double>(p.x()), unscale<double>(p.y())),
                         std::make_pair(first_lo + i, first_lo + n + i));
    }
    return first_lo;
}

// The vertical wall of one ring, over the two contiguous vertex runs append_ring_vertices laid
// down. One winding rule serves both a contour and a hole: the ring's own traversal direction
// already carries the orientation. Walking a CCW contour's bottom edge in +x gives
// (lo_i, lo_j, hi_j) the normal x cross z = -y, which points out of the part; a hole runs CW, so
// the same expression evaluates to the opposite side, which is again out of the material. This
// is the rule Slic3r::wall_strip uses, and it is right for the same reason.
void append_ring_walls(indexed_triangle_set &mesh, int first_lo, int n)
{
    for (int i = 0; i < n; ++i) {
        const int j    = (i + 1) % n;
        const int lo_i = first_lo + i,     lo_j = first_lo + j;
        const int hi_i = first_lo + n + i, hi_j = first_lo + n + j;
        mesh.indices.emplace_back(lo_i, lo_j, hi_j);
        mesh.indices.emplace_back(lo_i, hi_j, hi_i);
    }
}

// Turn the tesselator's flat triangle soup for `slice` into indexed faces over the vertices
// already appended for that slice. `top` picks the hi vertex run (and the up-facing winding);
// otherwise the lo run and the down-facing one.
void append_cap(indexed_triangle_set &mesh, PrismVertices &pv, const ExPolygons &slice,
                double z, bool top)
{
    // NORMALS_UP for a top cap, NORMALS_DOWN for a bottom one: the tesselator already emits the
    // triangle in the right order, so the winding below just follows it.
    const std::vector<Vec3d> tri = triangulate_expolygons_3d(slice, z, top ? NORMALS_UP : NORMALS_DOWN);
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
        int idx[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            auto it = pv.index.find(std::make_pair(tri[i + size_t(k)].x(), tri[i + size_t(k)].y()));
            if (it == pv.index.end()) {
                ok = false;
                break;
            }
            idx[k] = top ? it->second.second : it->second.first;
        }
        if (! ok) {
            // A tessCombine vertex: only possible on self-intersecting input. Drop the triangle
            // rather than invent a vertex the walls do not have (which is precisely the crack
            // this loft exists to avoid) and count it.
            ++pv.unmatched;
            continue;
        }
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2])
            continue; // degenerate, e.g. a zero-area sliver
        mesh.indices.emplace_back(idx[0], idx[1], idx[2]);
    }
}

// One layer -> one closed prism, appended to `mesh`.
void append_layer_prism(indexed_triangle_set &mesh, const ExPolygons &slice,
                        double z_lo, double z_hi, size_t &unmatched)
{
    if (slice.empty() || z_hi <= z_lo)
        return;

    PrismVertices pv;
    for (const ExPolygon &ex : slice) {
        if (ex.contour.points.size() >= 3) {
            const int first = append_ring_vertices(mesh, pv, ex.contour, z_lo, z_hi);
            append_ring_walls(mesh, first, int(ex.contour.points.size()));
        }
        for (const Polygon &hole : ex.holes) {
            if (hole.points.size() < 3)
                continue;
            const int first = append_ring_vertices(mesh, pv, hole, z_lo, z_hi);
            append_ring_walls(mesh, first, int(hole.points.size()));
        }
    }
    append_cap(mesh, pv, slice, z_lo, false);
    append_cap(mesh, pv, slice, z_hi, true);
    unmatched += pv.unmatched;
}

// The whole stack. Serial and ordered, so the vertex and face order is a pure function of the
// input - the determinism the bake asserts.
indexed_triangle_set prisms_to_mesh(const std::vector<ExPolygons> &slices,
                                    const std::vector<double>     &bottom_z,
                                    const std::vector<double>     &top_z,
                                    size_t                        &unmatched)
{
    indexed_triangle_set mesh;
    for (size_t i = 0; i < slices.size(); ++i)
        append_layer_prism(mesh, slices[i], bottom_z[i], top_z[i], unmatched);

    // Weld only EXACTLY coincident vertices: neighbouring prisms share a Z plane and, wherever
    // their contours share a point, that point's float coordinates are bit-identical (both sides
    // went through the same unscaled().cast<float>()). Nothing here is a tolerance merge, so a
    // weld can never pull two distinct contour points together and puncture the mesh.
    its_merge_vertices(mesh);
    its_remove_degenerate_faces(mesh);
    its_compactify_vertices(mesh);
    return mesh;
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
    // Two wall triangles per boundary point, plus a bottom and a top cap of about n-2 each over
    // the same n points: four per point. Deliberately a plain multiplication - a figure shown as
    // "about N" must not pretend to know the tesselation.
    return points * 4;
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

    // Each printed layer occupies [bottom_z, print_z] and becomes one closed prism spanning
    // exactly that band, so a variable or adaptive layer height and a subset that does not start
    // at the bed both come out right without a uniform grid having to be derived.
    size_t unmatched = 0;
    indexed_triangle_set mesh = prisms_to_mesh(slices, bottom_z, z, unmatched);
    if (unmatched > 0)
        rep.note = std::to_string(unmatched) + " cap triangle(s) dropped on self-intersecting input";

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
