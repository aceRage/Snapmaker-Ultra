// Slice baking, phase 1 - the gate for
// docs/superpowers/specs/2026-09-12-slice-bake-research.md.
//
// What "slice baking" is: take a SLICED object's outer wall - the loop the nozzle will actually
// follow, fuzzy skin and all - offset it by half its own extrusion width to get the printed
// boundary, fill it, and loft the stack into a watertight mesh. The bake is therefore a solid
// replica of what the slicer said the part would look like when printed.
//
// The owner's driving scenario, which scenario (b) below is the acceptance test for: slice at
// 0.3 mm WITH fuzzy skin, bake, then re-slice the bake at 0.12 mm WITHOUT fuzzy skin. The coarse
// fuzzy texture must survive into the fine re-slice - same amplitude, but now printed with 2.5x
// finer layers and no wall jitter of its own.
//
// The tests are tagged [slice_bake] so the whole set can be run on its own.

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SliceBake.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// ---------------------------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------------------------

DynamicPrintConfig bake_config(double layer_height, bool fuzzy)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               std::to_string(layer_height) },
        { "initial_layer_print_height", std::to_string(layer_height) },
        { "wall_loops",                 "2" },
        // Supports would put geometry in SupportLayers; scenario (d) turns them back on
        // deliberately to prove the bake never looks there.
        { "enable_support",             "0" },
        // Arc fitting rewrites the wall point set after the fact, which would make "the baked
        // boundary is the wall offset by width/2" only approximately true.
        { "enable_arc_fitting",         "0" },
        { "overhang_reverse",           "0" },
        { "spiral_mode",                "0" },
        { "wall_generator",             "classic" },
        // A brim and a skirt are exactly the things the bake must not pick up; leaving them on
        // would make "the bake's bbox is the object's bbox" a real assertion rather than a
        // tautology, so they stay on for the bbox checks.
        { "brim_type",                  "outer_only" },
        { "brim_width",                 "3" },
        { "skirt_loops",                "1" },
    });
    if (fuzzy) {
        config.set_deserialize_strict({
            { "fuzzy_skin",                "external" },
            { "fuzzy_skin_thickness",      "0.3" },
            { "fuzzy_skin_point_distance", "0.8" },
            { "fuzzy_skin_noise_type",     "classic" },
            { "fuzzy_skin_mode",           "displacement" },
            { "fuzzy_skin_first_layer",    "1" },
        });
    } else {
        config.set_deserialize_strict({{ "fuzzy_skin", "none" }});
    }
    return config;
}

// Slice one mesh and hand back the (single) PrintObject. `print` must outlive the returned
// pointer, so it is passed in by the caller.
const PrintObject *slice_one(Print &print, Model &model, TriangleMesh &&mesh, const DynamicPrintConfig &config,
                             const std::string &name)
{
    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(! print.objects().empty());
    return print.objects().front();
}

// ---------------------------------------------------------------------------------------------
// Mesh measurements
// ---------------------------------------------------------------------------------------------

BoundingBoxf3 bbox_of(const indexed_triangle_set &its)
{
    BoundingBoxf3 bb;
    for (const Vec3f &v : its.vertices)
        bb.merge(v.cast<double>());
    return bb;
}

// The distinct Z levels a lofted mesh has, to within `eps`.
//
// This is what "layer steps" means for a stair-stepped loft: every vertex sits on one of the
// band boundaries, so counting distinct Z values counts the boundaries. A stack of N layers has
// N+1 boundaries (the bed, then one per layer top), so the assertions below are stated in terms
// of the boundary count and the expected N is derived from it.
std::vector<double> distinct_z(const indexed_triangle_set &its, double eps = 1e-4)
{
    std::vector<double> zs;
    zs.reserve(its.vertices.size());
    for (const Vec3f &v : its.vertices)
        zs.push_back(double(v.z()));
    std::sort(zs.begin(), zs.end());
    std::vector<double> out;
    for (double z : zs)
        if (out.empty() || z - out.back() > eps)
            out.push_back(z);
    return out;
}

double mesh_volume(const indexed_triangle_set &its)
{
    // Signed volume by the divergence theorem; |result| so a flipped winding does not read as a
    // negative part.
    double v = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>();
        const Vec3d b = its.vertices[f(1)].cast<double>();
        const Vec3d c = its.vertices[f(2)].cast<double>();
        v += a.dot(b.cross(c));
    }
    return std::abs(v) / 6.;
}

// ---------------------------------------------------------------------------------------------
// Wall measurements on a sliced object
// ---------------------------------------------------------------------------------------------

void collect_outer(const ExtrusionEntity *entity, std::vector<Points> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_outer(child, out);
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        Points pts;
        for (const ExtrusionPath &path : loop->paths) {
            if (path.role() != erExternalPerimeter && path.role() != erOverhangPerimeter &&
                path.role() != erOverSupportPerimeter)
                continue;
            for (const Point &p : path.polyline.points)
                pts.push_back(p);
        }
        if (pts.size() >= 3)
            out.emplace_back(std::move(pts));
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        if ((path->role() == erExternalPerimeter || path->role() == erOverhangPerimeter ||
             path->role() == erOverSupportPerimeter) &&
            path->polyline.points.size() >= 3)
            out.emplace_back(path->polyline.points);
    }
}

std::vector<Points> outer_wall_points(const Layer &layer)
{
    std::vector<Points> out;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            collect_outer(entity, out);
    return out;
}

// How far a layer's outer wall departs from its NOMINAL wall plane, in mm.
//
// The models here are axis-aligned squares, so an unfuzzed wall is a rectangle with four sides,
// each of which is a plane x = const or y = const. The deviation of a wall point is its signed
// distance to the plane of the side it belongs to.
//
// The nominal plane is the MEDIAN of the points on that side, not the extreme. That distinction
// is the whole point of this rewrite: taking the bounding box puts the plane at the outermost
// fuzz excursion (+thickness), so an inward excursion (-thickness) reads as 2x thickness away
// from it, and the measured max is 2T rather than T. The median sits on the un-fuzzed wall line,
// which is where the geometry actually is, so the measured max is bounded by the fuzz amplitude
// itself plus the half line width by which the re-slice's centreline can wander.
//
// Assignment to a side is by which of the four the point is nearest, using a first pass over the
// bounding box purely to know roughly where the sides are. A corner point is ambiguous and is
// dropped (its distance to both of its sides is near zero either way, so it carries no signal).
//
// Returned as {mean, max} of |signed distance|. An unfuzzed rectangle gives ~0 for both.
struct Deviation { double mean = 0.; double max = 0.; size_t n = 0; };

// The per-point signed deviations from the nominal wall planes, in wall order. Side 0 = x_min,
// 1 = x_max, 2 = y_min, 3 = y_max; a point too near a corner gets side -1 and is skipped.
struct WallSamples
{
    std::vector<double> dev;    // signed distance to the point's own nominal plane, outward +
    std::vector<int>    side;
};

WallSamples wall_samples(const std::vector<Points> &walls, double corner_margin = 1.0)
{
    WallSamples out;
    double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
    std::vector<Vec2d> pts;
    for (const Points &w : walls)
        for (const Point &pt : w) {
            const double x = unscaled(double(pt.x())), y = unscaled(double(pt.y()));
            pts.emplace_back(x, y);
            x_min = std::min(x_min, x); x_max = std::max(x_max, x);
            y_min = std::min(y_min, y); y_max = std::max(y_max, y);
        }
    if (pts.size() < 8 || x_max - x_min < 1. || y_max - y_min < 1.)
        return out;

    // Assign each point to the side it is nearest, dropping the corners.
    std::vector<int> side(pts.size(), -1);
    std::vector<double> on[4];
    for (size_t i = 0; i < pts.size(); ++i) {
        const double d[4] = { pts[i].x() - x_min, x_max - pts[i].x(),
                              pts[i].y() - y_min, y_max - pts[i].y() };
        int    best = 0;
        double bestd = d[0];
        for (int k = 1; k < 4; ++k)
            if (d[k] < bestd) { bestd = d[k]; best = k; }
        // Near a corner two sides are both close; require the runner-up to be clearly further.
        double second = 1e30;
        for (int k = 0; k < 4; ++k)
            if (k != best) second = std::min(second, d[k]);
        if (second < corner_margin)
            continue;
        side[i] = best;
        on[best].push_back(best < 2 ? pts[i].x() : pts[i].y());
    }

    // The nominal plane of each side is the median of its own points.
    double plane[4] = {0., 0., 0., 0.};
    for (int k = 0; k < 4; ++k) {
        if (on[k].empty())
            return out;
        std::sort(on[k].begin(), on[k].end());
        plane[k] = on[k][on[k].size() / 2];
    }

    for (size_t i = 0; i < pts.size(); ++i) {
        if (side[i] < 0)
            continue;
        const int k = side[i];
        const double c = k < 2 ? pts[i].x() : pts[i].y();
        // Outward is -x for side 0 and -y for side 2, +x / +y for 1 and 3.
        const double signed_dev = (k == 0 || k == 2) ? (plane[k] - c) : (c - plane[k]);
        out.dev.push_back(signed_dev);
        out.side.push_back(k);
    }
    return out;
}

Deviation wall_deviation(const std::vector<Points> &walls)
{
    const WallSamples s = wall_samples(walls);
    Deviation d;
    if (s.dev.empty())
        return d;
    double sum = 0., mx = 0.;
    for (double v : s.dev) {
        sum += std::abs(v);
        mx   = std::max(mx, std::abs(v));
    }
    d.mean = sum / double(s.dev.size());
    d.max  = mx;
    d.n    = s.dev.size();
    return d;
}

Deviation wall_deviation(const Layer &layer) { return wall_deviation(outer_wall_points(layer)); }

// A layer's outer wall as a DEVIATION profile: the signed distance from the nominal wall plane,
// sampled at `k` evenly spaced positions around the rectangle's perimeter.
//
// This replaces the centroid-to-wall radial profile the first draft used. On a square section a
// centroid radius swings by about +/-2 mm between the middle of a side and a corner - seven times
// the 0.3 mm fuzz being looked for - so two layers with completely different fuzz still produced
// near-identical profiles, and the in-band vs cross-band contrast was invisible (0.546 vs 0.554).
// Measuring the deviation from the LOCAL wall plane removes that geometric term entirely: an
// unfuzzed square profiles as all zeros, and what is left in the profile is the fuzz and nothing
// else.
//
// The parameter is the point's position along the perimeter of the nominal rectangle, so two
// layers cut from the same baked band land their samples in the same bins even though their
// point counts differ.
std::vector<double> band_profile(const Layer &layer, size_t k = 240)
{
    const std::vector<Points> walls = outer_wall_points(layer);
    double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
    std::vector<Vec2d> pts;
    for (const Points &w : walls)
        for (const Point &pt : w) {
            const double x = unscaled(double(pt.x())), y = unscaled(double(pt.y()));
            pts.emplace_back(x, y);
            x_min = std::min(x_min, x); x_max = std::max(x_max, x);
            y_min = std::min(y_min, y); y_max = std::max(y_max, y);
        }
    if (pts.size() < 8 || x_max - x_min < 1. || y_max - y_min < 1.)
        return {};

    const WallSamples s = wall_samples(walls);
    if (s.dev.empty())
        return {};

    // Re-walk the points in the same order wall_samples did, to recover each sample's XY and so
    // its perimeter position. wall_samples skips corner points, so the walk has to skip the same
    // ones - it is driven by the side vector it returned.
    const double w = x_max - x_min, h = y_max - y_min;
    const double perim = 2. * (w + h);
    std::vector<double> sum(k, 0.);
    std::vector<int>    cnt(k, 0);

    size_t si = 0;
    for (size_t i = 0; i < pts.size() && si < s.dev.size(); ++i) {
        // wall_samples emitted one entry per non-corner point, in this same order; a point that
        // was dropped has no entry, and the two walks stay in step because the side assignment
        // is a pure function of the point.
        const double d[4] = { pts[i].x() - x_min, x_max - pts[i].x(),
                              pts[i].y() - y_min, y_max - pts[i].y() };
        int    best = 0;
        double bestd = d[0];
        for (int q = 1; q < 4; ++q)
            if (d[q] < bestd) { bestd = d[q]; best = q; }
        double second = 1e30;
        for (int q = 0; q < 4; ++q)
            if (q != best) second = std::min(second, d[q]);
        if (second < 1.0)
            continue;   // dropped by wall_samples too

        // Perimeter position: side 2 (y_min) runs +x from the origin corner, then side 1 (x_max)
        // runs +y, then side 3 (y_max) runs -x, then side 0 (x_min) runs -y.
        double t = 0.;
        switch (best) {
        case 2: t = (pts[i].x() - x_min); break;
        case 1: t = w + (pts[i].y() - y_min); break;
        case 3: t = w + h + (x_max - pts[i].x()); break;
        default: t = 2. * w + h + (y_max - pts[i].y()); break;
        }
        const size_t bin = std::min(k - 1, size_t(std::max(0., t) / perim * double(k)));
        sum[bin] += s.dev[si];
        ++cnt[bin];
        ++si;
    }

    std::vector<double> prof(k, 0.);
    for (size_t i = 0; i < k; ++i)
        prof[i] = cnt[i] > 0 ? sum[i] / double(cnt[i]) : 0.;
    // A bin with no sample inherits its neighbour rather than reading as a 0 mm deviation.
    for (size_t i = 0; i < k; ++i)
        if (cnt[i] == 0)
            prof[i] = prof[(i + k - 1) % k];
    return prof;
}

double profile_rms_diff(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size() || a.empty())
        return std::numeric_limits<double>::infinity();
    double s = 0.;
    for (size_t i = 0; i < a.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(s / double(a.size()));
}

} // namespace

// =============================================================================================
// (a) 20 mm cube at 0.2 mm: watertight, 100 layer steps, volume within 2%
// =============================================================================================

TEST_CASE("slice bake: a 20 mm cube at 0.2 mm lofts to a watertight 100-step solid", "[slice_bake]")
{
    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cube(20., 20., 20.)),
                                          bake_config(0.2, false), "cube20");

    REQUIRE(slice_bake_available(*object));

    SliceBakeOptions opts;          // all layers, no gap closing
    SliceBakeReport  rep;
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);

    REQUIRE(! mesh.indices.empty());

    // -- watertight ---------------------------------------------------------------------------
    INFO("open edges: " << its_num_open_edges(mesh));
    CHECK(its_num_open_edges(mesh) == 0);
    CHECK(rep.watertight);

    // -- 100 layer steps ----------------------------------------------------------------------
    // 20 mm at 0.2 mm is 100 layers, i.e. 101 Z boundaries: the bed and each layer's top.
    const std::vector<double> zs = distinct_z(mesh);
    INFO("distinct Z levels: " << zs.size() << " (first " << zs.front() << ", last " << zs.back() << ")");
    CHECK(zs.size() == 101);
    CHECK(rep.layers_baked == 100);

    // The stack spans the cube's full height.
    CHECK(zs.front() == Approx(0.).margin(1e-3));
    CHECK(zs.back()  == Approx(20.).margin(1e-3));

    // -- volume within 2% of the cube plus the wall's half-width growth ------------------------
    // The bake's boundary is the wall CENTRELINE offset outward by width/2. The centreline of
    // the outer wall sits half a line width inside the slice, so offsetting it back out by the
    // same half width lands on the slice outline - the cube's own face, to within the
    // elephant-foot/first-layer compensation and the corner rounding the offset's round join
    // leaves. So the expected volume is the nominal 8000 mm^3, and the 2% window absorbs those.
    const double vol = mesh_volume(mesh);
    const double nominal = 20. * 20. * 20.;
    INFO("baked volume " << vol << " mm^3 vs nominal " << nominal << " ("
         << (100. * (vol - nominal) / nominal) << "%)");
    CHECK(vol == Approx(nominal).epsilon(0.02));

    // The bake is positioned in the object's frame: X/Y centred on the volume's own origin, which
    // for its_make_cube is the corner at (0,0,0).
    const BoundingBoxf3 bb = bbox_of(mesh);
    INFO("bbox " << bb.min.transpose() << " .. " << bb.max.transpose());
    CHECK(bb.size().x() == Approx(20.).margin(0.3));
    CHECK(bb.size().y() == Approx(20.).margin(0.3));
    CHECK(bb.size().z() == Approx(20.).margin(1e-3));
}

// =============================================================================================
// (b) THE OWNER'S SCENARIO
//     0.3 mm + fuzzy skin -> bake -> re-slice at 0.12 mm with fuzzy OFF
// =============================================================================================

TEST_CASE("slice bake: 0.3 mm fuzzy skin survives a 0.12 mm re-slice of the bake", "[slice_bake]")
{
    // ---- 1. slice the cube at 0.3 mm WITH fuzzy skin ----------------------------------------
    Print  src_print;
    Model  src_model;
    const PrintObject *src = slice_one(src_print, src_model, TriangleMesh(its_make_cube(20., 20., 20.)),
                                       bake_config(0.3, true), "cube20_fuzzy");

    // The source really is fuzzed: its walls depart from a rectangle by something on the order of
    // half the 0.3 mm thickness. Without this the rest of the test could pass on a flat cube.
    const Deviation src_dev = wall_deviation(*src->get_layer(20));
    INFO("source (0.3 mm, fuzzy) wall deviation: mean " << src_dev.mean << " max " << src_dev.max);
    REQUIRE(src_dev.n > 0);
    REQUIRE(src_dev.max > 0.1);

    // ---- 2. bake -----------------------------------------------------------------------------
    SliceBakeReport  rep;
    SliceBakeOptions opts;
    const indexed_triangle_set baked = slice_bake_to_mesh(*src, opts, &rep);
    REQUIRE(! baked.indices.empty());
    CHECK(its_num_open_edges(baked) == 0);

    // 20 mm at 0.3 mm is 66 full layers plus a 0.2 mm remainder -> 67 layers.
    INFO("baked " << rep.layers_baked << " layers, " << rep.triangles << " triangles");
    CHECK(rep.layers_baked >= 66);
    CHECK(rep.layers_baked <= 68);

    // ---- 3. re-slice the BAKED mesh at 0.12 mm with fuzzy skin OFF ---------------------------
    Print  re_print;
    Model  re_model;
    TriangleMesh baked_mesh(baked);
    const PrintObject *re = slice_one(re_print, re_model, std::move(baked_mesh),
                                      bake_config(0.12, false), "cube20_baked");
    REQUIRE(re->layer_count() > 100);

    // ---- 4a. the texture is there, at roughly the fuzzy thickness ----------------------------
    // Measured over the middle of the part, away from the first layer (elephant foot) and the top
    // (the last band may be a partial layer).
    double max_dev = 0.;
    double sum_max = 0.;
    size_t n_layers = 0;
    for (size_t i = 20; i + 20 < re->layer_count(); ++i) {
        const Deviation d = wall_deviation(*re->get_layer(int(i)));
        if (d.n == 0)
            continue;
        max_dev = std::max(max_dev, d.max);
        sum_max += d.max;
        ++n_layers;
    }
    REQUIRE(n_layers > 50);
    const double mean_max = sum_max / double(n_layers);
    INFO("re-sliced (0.12 mm, fuzzy OFF) deviation: overall max " << max_dev
         << ", mean per-layer max " << mean_max << " (fuzzy thickness was 0.3)");

    // The bound, derived rather than fitted.
    //
    // The source's fuzz displaces the outer wall CENTRELINE by noise * fuzzy_skin_thickness, and
    // the classic noise is uniform on [-1, +1], so the centreline excursion is at most T = 0.3 mm
    // either side of the nominal wall plane. The bake offsets that centreline outward by the
    // path's own width/2, a CONSTANT, which shifts the whole wall out without changing its
    // amplitude. The re-slice then cuts that surface and lays its own external perimeter half a
    // line width inside it - again a constant shift. Both constants are absorbed by measuring
    // against the median wall plane, so what is left is the fuzz amplitude T, plus at most half a
    // line width w/2 of slack for where the re-slice's own centreline lands on a corner or on a
    // step between two baked bands.
    //
    //     max deviation <= T + w/2 = 0.3 + 0.42/2 = 0.51 mm
    //
    // and it must be at least half the amplitude, or the texture did not survive the bake at all.
    const double T = 0.3, w_nominal = 0.42;
    const double bound = T + 0.5 * w_nominal;
    INFO("bound: fuzz thickness " << T << " + half line width " << (0.5 * w_nominal) << " = " << bound);
    CHECK(max_dev > 0.5 * T);
    CHECK(max_dev < bound);

    // ---- 4b. the deviation profile along Z is piecewise-constant in 0.3 mm bands -------------
    // This is the half that proves the texture came from the SOURCE's 0.3 mm layers rather than
    // from noise the re-slice invented. Two 0.12 mm layers whose Z falls inside one 0.3 mm source
    // band were cut from the same baked band, so their walls are the same closed curve and their
    // deviation profiles agree to a hair. Two layers straddling a band boundary were cut from
    // DIFFERENT source layers, each with its own independent fuzz, so their profiles differ by
    // something on the order of the fuzz itself.
    //
    // Both quantities are collected and compared as an in-band vs cross-band contrast rather than
    // against absolute constants: what is being asserted is that the bands exist.
    std::vector<double> in_band, cross_band;
    const double band = 0.3;
    std::vector<double>          prev_prof;
    double                       prev_z = -1.;
    for (size_t i = 20; i + 20 < re->layer_count(); ++i) {
        const Layer *layer = re->get_layer(int(i));
        const std::vector<double> prof = band_profile(*layer);
        if (prof.empty()) { prev_prof.clear(); continue; }
        if (! prev_prof.empty()) {
            // Which source band each layer's MIDDLE falls in. Using the middle rather than
            // print_z keeps a layer whose top grazes a boundary on the side it was mostly cut
            // from.
            const double z_mid      = layer->print_z - 0.5 * layer->height;
            const double prev_mid   = prev_z;
            const long   band_now   = long(std::floor(z_mid / band));
            const long   band_prev  = long(std::floor(prev_mid / band));
            const double d          = profile_rms_diff(prof, prev_prof);
            if (std::isfinite(d)) {
                if (band_now == band_prev) in_band.push_back(d);
                else                       cross_band.push_back(d);
            }
        }
        prev_prof = prof;
        prev_z    = layer->print_z - 0.5 * layer->height;
    }

    REQUIRE(in_band.size() > 20);
    REQUIRE(cross_band.size() > 20);
    auto mean_of = [](const std::vector<double> &v) {
        double s = 0.; for (double x : v) s += x; return s / double(v.size());
    };
    const double in_mean    = mean_of(in_band);
    const double cross_mean = mean_of(cross_band);
    INFO("wall-deviation profile RMS difference between adjacent 0.12 mm layers: within a 0.3 mm band "
         << in_mean << " mm (" << in_band.size() << " pairs), across a band boundary "
         << cross_mean << " mm (" << cross_band.size() << " pairs)");

    // Adjacent layers inside one band were cut from the SAME baked prism, so the only thing that
    // can differ between their profiles is where the re-slice happened to put its own wall points
    // - the two layers sample one identical curve at different parameters.
    //
    // The size of that resampling term is set by the bin width, not by the geometry. The profile
    // has 240 bins over an ~80 mm perimeter, so a bin is about 0.33 mm wide, and inside one bin
    // the fuzzed wall can swing by the fuzz slope over that distance. The fuzz is sampled every
    // fuzzy_skin_point_distance = 0.8 mm with an amplitude of +/-T, so over a third of a sample
    // spacing the wall moves by roughly (0.33 / 0.8) * T ~ 0.4 T, and two independent samplings
    // of it differ in RMS by a fraction of that. Half the fuzz amplitude is the ceiling:
    //
    //     in-band RMS <= 0.5 * T = 0.15 mm
    //
    // The load-bearing assertion is the CONTRAST below, not this one - this only confirms the
    // in-band figure is small in absolute terms rather than merely smaller than the cross-band.
    CHECK(in_mean < 0.5 * T);
    // ...and the jump at a band boundary is several times larger, because the two layers were cut
    // from source layers whose fuzz is independent. A 3x ratio is well clear of the resampling
    // noise while leaving room for the bands that happen to fuzz similarly.
    CHECK(cross_mean > 3. * in_mean);
}

// =============================================================================================
// (c) cylinder: step count and radius
// =============================================================================================

TEST_CASE("slice bake: a cylinder keeps its step count and its radius", "[slice_bake]")
{
    const double r = 8., h = 12., lh = 0.2;

    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cylinder(r, h)),
                                          bake_config(lh, false), "cyl");

    SliceBakeReport  rep;
    SliceBakeOptions opts;
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);
    REQUIRE(! mesh.indices.empty());
    CHECK(its_num_open_edges(mesh) == 0);

    // 12 mm at 0.2 mm is 60 layers -> 61 Z boundaries.
    const std::vector<double> zs = distinct_z(mesh);
    INFO("cylinder: " << rep.layers_baked << " layers baked, " << zs.size() << " Z levels");
    CHECK(rep.layers_baked == size_t(std::lround(h / lh)));
    CHECK(zs.size() == size_t(std::lround(h / lh)) + 1);

    // Radius: fit to a horizontal band of vertices in the middle of the part. The bake's boundary
    // is the wall centreline offset out by width/2, which lands back on the slice outline, so the
    // radius should come back as the source radius to within the perimeter's half width - and
    // slightly under it, because a polygonal slice of a circle is inscribed.
    Vec2d c(0., 0.);
    std::vector<Vec2d> ring;
    for (const Vec3f &v : mesh.vertices)
        if (std::abs(double(v.z()) - 0.5 * h) < 0.3)
            ring.emplace_back(double(v.x()), double(v.y()));
    REQUIRE(ring.size() > 20);
    for (const Vec2d &p : ring) c += p;
    c /= double(ring.size());

    double r_sum = 0., r_max = 0., r_min = 1e30;
    for (const Vec2d &p : ring) {
        const double d = (p - c).norm();
        r_sum += d; r_max = std::max(r_max, d); r_min = std::min(r_min, d);
    }
    const double r_fit = r_sum / double(ring.size());
    INFO("cylinder radius: fitted " << r_fit << " (min " << r_min << ", max " << r_max
         << ") vs source " << r);
    // Within half a nominal 0.42 mm line width of the source radius, as the spec asks.
    CHECK(std::abs(r_fit - r) < 0.25);
}

// =============================================================================================
// (d) supports in the source slice do not appear in the bake
// =============================================================================================

TEST_CASE("slice bake: supports, brim and skirt are never baked", "[slice_bake]")
{
    // A T: a 4x4x10 mm stem carrying a 20x20x2 mm slab, whose whole underside is an overhang the
    // slicer will support. If any support geometry reached the bake, it would show up under the
    // slab - i.e. inside the object's own footprint but far below its top - and the bake's
    // bounding box would not be the object's.
    indexed_triangle_set its = its_make_cube(4., 4., 10.);
    {
        indexed_triangle_set slab = its_make_cube(20., 20., 2.);
        for (Vec3f &v : slab.vertices) {
            v.x() -= 8.f;   // centre the slab on the stem
            v.y() -= 8.f;
            v.z() += 10.f;
        }
        its_merge(its, slab);
    }

    DynamicPrintConfig config = bake_config(0.2, false);
    config.set_deserialize_strict({
        // Supports ON: the thing the bake must ignore.
        { "enable_support",        "1" },
        { "support_type",          "normal(auto)" },
        { "support_threshold_angle", "45" },
        // Brim and skirt are on from bake_config and stay on.
    });

    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its), config, "tee");

    // The slice really did produce support - otherwise this asserts nothing.
    INFO("support layers: " << object->support_layer_count());
    REQUIRE(object->support_layer_count() > 0);

    SliceBakeReport  rep;
    SliceBakeOptions opts;
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);
    REQUIRE(! mesh.indices.empty());

    const BoundingBoxf3 bb = bbox_of(mesh);
    INFO("bake bbox " << bb.min.transpose() << " .. " << bb.max.transpose());

    // The bake spans the OBJECT: 20x20 in XY (the slab) and 12 in Z. A skirt would blow the XY
    // size out by tens of millimetres; a brim by twice its 3 mm width; support under the slab
    // would fill the void under the slab but not change the bbox, so the volume check below covers that.
    CHECK(bb.size().x() == Approx(20.).margin(0.5));
    CHECK(bb.size().y() == Approx(20.).margin(0.5));
    CHECK(bb.size().z() == Approx(12.).margin(0.05));

    // And the volume is the T's own, not the T plus a block of support. The T is
    // 4*4*10 + 20*20*2 = 960 mm^3; a support-filled underside would add most of
    // (20*20 - 4*4) * 10 = 3840 mm^3 on top of it.
    const double vol = mesh_volume(mesh);
    INFO("bake volume " << vol << " mm^3 (the T alone is 960; with the support void filled it "
         "would be about 4800)");
    CHECK(vol < 1300.);
    CHECK(vol > 800.);
}

// =============================================================================================
// Determinism and the options
// =============================================================================================

TEST_CASE("slice bake: the same slice bakes to the same mesh twice", "[slice_bake]")
{
    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cube(10., 10., 5.)),
                                          bake_config(0.2, true), "cube10_fuzzy");

    SliceBakeOptions opts;
    const indexed_triangle_set a = slice_bake_to_mesh(*object, opts);
    const indexed_triangle_set b = slice_bake_to_mesh(*object, opts);

    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices.size() == b.indices.size());
    for (size_t i = 0; i < a.vertices.size(); ++i)
        REQUIRE(a.vertices[i] == b.vertices[i]);
    for (size_t i = 0; i < a.indices.size(); ++i)
        REQUIRE(a.indices[i] == b.indices[i]);
}

TEST_CASE("slice bake: a layer subset bakes only those layers", "[slice_bake]")
{
    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cube(10., 10., 10.)),
                                          bake_config(0.2, false), "cube10");

    SliceBakeOptions opts;
    opts.layer_begin = 10;
    opts.layer_end   = 30;
    SliceBakeReport rep;
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);

    REQUIRE(! mesh.indices.empty());
    CHECK(rep.layers_baked == 20);
    const BoundingBoxf3 bb = bbox_of(mesh);
    // Layers 10..29 print from z = 2.0 to z = 6.0 at 0.2 mm.
    INFO("subset bbox z " << bb.min.z() << " .. " << bb.max.z());
    CHECK(bb.min.z() == Approx(2.0).margin(0.05));
    CHECK(bb.max.z() == Approx(6.0).margin(0.05));
}

TEST_CASE("slice bake: closing the gaps does not break watertightness", "[slice_bake]")
{
    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cube(10., 10., 5.)),
                                          bake_config(0.2, true), "cube10_fuzzy_closed");

    SliceBakeOptions opts;
    opts.close_gaps_radius = 0.1;
    SliceBakeReport rep;
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);

    REQUIRE(! mesh.indices.empty());
    CHECK(its_num_open_edges(mesh) == 0);
    // Closing rounds the fuzz off a little, so the closed bake is never SMALLER than the open one
    // by more than the radius' worth of erosion - it is a dilate followed by an erode.
    SliceBakeOptions plain;
    const indexed_triangle_set open_mesh = slice_bake_to_mesh(*object, plain);
    INFO("volume open " << mesh_volume(open_mesh) << " closed " << mesh_volume(mesh));
    CHECK(mesh_volume(mesh) >= mesh_volume(open_mesh) * 0.98);
}

// =============================================================================================
// Timing: a 100 mm part at 0.2 mm
// =============================================================================================
//
// The spec asks for a figure on a benchy-class part, i.e. ~100 mm tall at the default layer
// height: 500 layers, which is where the bake's cost actually lives. It reports the wall time and
// the triangle count rather than asserting a number, because both are machine-dependent; the only
// assertion is a generous ceiling that catches an accidental quadratic, not a slow PC.
TEST_CASE("slice bake: a 100 mm part at 0.2 mm bakes in reasonable time", "[slice_bake]")
{
    Print print;
    Model model;
    const PrintObject *object = slice_one(print, model, TriangleMesh(its_make_cube(100., 100., 100.)),
                                          bake_config(0.2, false), "cube100");

    SliceBakeOptions opts;
    SliceBakeReport  rep;

    const auto t0 = std::chrono::steady_clock::now();
    const indexed_triangle_set mesh = slice_bake_to_mesh(*object, opts, &rep);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    REQUIRE(! mesh.indices.empty());

    const double open_t0 = secs;
    const auto   t2      = std::chrono::steady_clock::now();
    const size_t open    = its_num_open_edges(mesh);
    const double check_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();

    // BENCHMARK is the line to read out of the test log.
    WARN("BENCHMARK 100 mm cube @ 0.2 mm: " << rep.layers_baked << " layers, "
         << rep.triangles << " triangles, " << rep.vertices << " vertices, bake "
         << open_t0 << " s, watertightness check " << check_s << " s, open edges " << open);

    CHECK(open == 0);
    CHECK(rep.layers_baked == 500);
    // A generous ceiling: the bake is a Clipper union per layer plus a linear loft, so 500 layers
    // of a trivial footprint should be a small number of seconds. 60 s only fires on an
    // algorithmic regression.
    CHECK(secs < 60.);
}
