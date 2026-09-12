// ImageMap Phase 4, step 1: side-wall dithering. See ImageRowWalls.hpp for the design and for
// why the split happens at G-code time rather than inside PerimeterGenerator.

#include "ImageRowWalls.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>

#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ImageFill.hpp"
#include "MixedFilament.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

namespace {

// Same shape as Fill.cpp's two one-shot loggers: a fallback that fires silently is a fallback
// nobody can diagnose from a log, and the GUI tooltip is not where a maintainer looks.
void log_image_row_wall_no_color_once()
{
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        BOOST_LOG_TRIVIAL(warning) << "Image row (ImageWeighted wall dithering): the projection declined a colour "
                                      "along a whole outer perimeter - that wall prints with the region's nominal "
                                      "filament instead of a dithered run sequence. A cylindrical projection whose "
                                      "axis passes through the wall, or a projection aimed away from this face, "
                                      "does this. See docs/superpowers/specs/2026-09-12-imagemap-phase4-walls.md.";
}

// Resolves the ModelVolume actually backing `region`, via the same volume<->region map
// PrintObjectRegions builds for painted regions. Transcribed from Fill.cpp's
// image_row_owning_volume() rather than shared, because that one lives in Fill.cpp's anonymous
// namespace; keeping this file self-contained avoids reshaping Fill.cpp's own translation unit
// (which other work touches) for one 12-line helper.
const ModelVolume *wall_owning_volume(const PrintObject &object, const PrintRegion &region)
{
    if (const PrintObjectRegions *shared = object.shared_regions(); shared != nullptr)
        for (const PrintObjectRegions::LayerRangeRegions &range : shared->layer_ranges)
            for (const PrintObjectRegions::VolumeRegion &vr : range.volume_regions)
                if (vr.region == &region && vr.model_volume != nullptr && vr.model_volume->is_model_part())
                    return vr.model_volume;
    const ModelObject *mo = object.model_object();
    if (mo != nullptr)
        for (const ModelVolume *v : mo->volumes)
            if (v != nullptr && v->is_model_part())
                return v;
    return nullptr;
}

// Flattens a perimeter entity into the ordered ExtrusionPaths the nozzle actually walks.
// An ExtrusionLoop's paths are already in seam order once place_seam() has rotated it, and a
// loop's last point equals its first, so walking `paths` front to back traces the whole loop
// starting and ending at the seam - which is exactly what the split needs.
bool collect_wall_paths(const ExtrusionEntity &entity, ExtrusionPaths &out)
{
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        out = loop->paths;
    } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        out = mp->paths;
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        out.assign(1, *path);
    } else {
        return false;
    }
    return !out.empty();
}

// The concatenated polyline of `paths`, with the duplicate point shared by consecutive paths
// dropped, so arc length along the result is arc length along the wall.
Polyline concatenated_polyline(const ExtrusionPaths &paths)
{
    Polyline pl;
    for (const ExtrusionPath &p : paths) {
        if (p.polyline.points.size() < 2)
            continue;
        if (pl.points.empty())
            pl.points = p.polyline.points;
        else
            pl.points.insert(pl.points.end(), p.polyline.points.begin() + 1, p.polyline.points.end());
    }
    return pl;
}

// The wall's own outward normal at the segment (a, b), in PRINT space, as a 3D unit vector.
//
// A perimeter is extruded in the XY plane, so the wall's surface normal is horizontal and
// perpendicular to the direction of travel. Which of the two perpendiculars points OUT of the
// part is fixed by the loop's winding: PerimeterGenerator calls make_counter_clockwise() on every
// loop it emits (PerimeterGenerator.cpp:380 and :703), and for a counter-clockwise contour the
// outward normal is the tangent rotated by -90 degrees, i.e. (dy, -dx).
//
// `ccw` is false for a HOLE loop, whose winding is reversed and whose "outward" (into the solid)
// normal is therefore the other perpendicular. A hole's wall faces the inside of a bore, which is
// exactly the surface ImageFillParams::axis_negative selects for a cylindrical projection - so
// getting this sign right is what makes a bore's wall sample its own side of the image rather
// than the far side's.
Vec3d wall_outward_normal_print(const Point &a, const Point &b, bool ccw)
{
    const Vec2d t(unscale<double>(b.x() - a.x()), unscale<double>(b.y() - a.y()));
    const double len = t.norm();
    if (len < 1e-12)
        return Vec3d(0., 0., 0.);
    const Vec2d n = ccw ? Vec2d(t.y(), -t.x()) / len : Vec2d(-t.y(), t.x()) / len;
    return Vec3d(n.x(), n.y(), 0.);
}

} // namespace

bool image_row_wall_entity_is_claimed(const ExtrusionEntity &entity, bool split_first_inner)
{
    // inset_idx is what PerimeterGenerator stamps on each loop (0 = the outer wall). Some
    // entities never get one (it defaults to -1), so fall back to the role the way GCode.cpp's
    // own outer/inner splitter does - an entity with any erExternalPerimeter path is an outer
    // wall. An outer loop's paths are a MIX of erExternalPerimeter and erOverhangPerimeter
    // (PerimeterGenerator retags overhang sub-segments), so "any" is right and "all" would miss.
    auto paths_are_outer = [](const ExtrusionPaths &paths, int inset_idx) -> bool {
        if (inset_idx == 0)
            return true;
        if (inset_idx > 0)
            return false;
        for (const ExtrusionPath &p : paths)
            if (p.role() == erExternalPerimeter)
                return true;
        return false;
    };
    int            inset = entity.inset_idx;
    ExtrusionPaths paths;
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        paths = loop->paths;
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        paths = mp->paths;
    else if (const auto *p = dynamic_cast<const ExtrusionPath *>(&entity))
        paths.assign(1, *p);
    else
        return false;

    if (paths_are_outer(paths, inset))
        return true;
    return split_first_inner && inset == 1;
}

unsigned int image_row_wall_configured_virtual_id(const PrintObject &object, const PrintRegion &region)
{
    const Print *print = object.print();
    if (print == nullptr)
        return 0;
    const PrintRegionConfig &cfg = region.config();
    // Same precedence PrintRegion::extruder() uses: a region that names its own outer-wall
    // filament is asking for the OUTER wall specifically, which is exactly the surface this
    // feature paints, so that key wins when it is set.
    const int raw = cfg.outer_wall_filament.value > 0 ? cfg.outer_wall_filament.value : cfg.wall_filament.value;
    const unsigned int configured_id = unsigned(std::max(0, raw));
    const size_t num_physical = print->config().filament_diameter.size();
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    if (!mixed_mgr.is_mixed(configured_id, num_physical))
        return 0;
    const MixedFilament *mf = mixed_mgr.mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr || !mf->enabled || mf->distribution_mode != int(MixedFilament::ImageWeighted) ||
        mf->image_fill_ref.empty())
        return 0;
    return configured_id;
}

std::vector<unsigned int> image_row_wall_candidate_filaments(const PrintObject &object, const PrintRegion &region,
                                                             size_t num_physical)
{
    std::vector<unsigned int> out;
    const Print *print = object.print();
    if (print == nullptr || num_physical == 0)
        return out;
    const unsigned int configured_id = image_row_wall_configured_virtual_id(object, region);
    if (configured_id == 0)
        return out;
    const MixedFilament *mf = print->mixed_filament_manager().mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr)
        return out;
    // Exactly the filter image_row_wall_context_for_region() applies to build candidate_ids, so
    // the set registered here and the set the runs are actually drawn from cannot diverge.
    std::vector<unsigned int> allowed =
        MixedFilamentManager::decode_gradient_component_ids(mf->gradient_component_ids, num_physical);
    if (allowed.size() < 2)
        allowed = {mf->component_a, mf->component_b};
    const std::vector<std::string> &filament_colours = print->config().filament_colour.values;
    for (unsigned int id : allowed)
        if (id >= 1 && id <= num_physical && id <= filament_colours.size() &&
            std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    if (out.size() < 2)
        out.clear();   // the context builder would refuse this row too; register nothing.
    return out;
}

namespace {

// The shared body of both public context builders: everything that depends only on WHICH row was
// named, not on which config key named it.
bool build_context_for_row(const PrintObject &object, const PrintRegion &region, unsigned int configured_id,
                           float flow_width_mm, ImageRowWallContext &ctx)
{
    const Print *print = object.print();
    if (print == nullptr || configured_id == 0)
        return false;

    const size_t                num_physical = print->config().filament_diameter.size();
    const MixedFilamentManager &mixed_mgr    = print->mixed_filament_manager();
    const MixedFilament        *mf           = mixed_mgr.mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr)
        return false; // already checked above; defensive only.

    const std::string raw = MixedFilamentManager::decode_image_fill_ref(mf->image_fill_ref);
    if (raw.empty() || !ImageFillParams::from_string(raw, ctx.params))
        return false;

    // The row's allowed filaments, exactly as phase 3 reads them: gradient_component_ids is an
    // ImageWeighted row's "allowed" list, just consumed differently from a gradient row's.
    std::vector<unsigned int> allowed =
        MixedFilamentManager::decode_gradient_component_ids(mf->gradient_component_ids, num_physical);
    if (allowed.size() < 2)
        allowed = {mf->component_a, mf->component_b};

    const std::vector<std::string> &filament_colours = print->config().filament_colour.values;
    for (unsigned int id : allowed) {
        if (id < 1 || id > num_physical || id > filament_colours.size())
            continue;
        ctx.candidate_ids.push_back(int(id));
        ctx.candidate_colors.push_back(MixedFilamentManager::hex_to_srgb01(filament_colours[id - 1]));
    }
    if (ctx.candidate_ids.size() < 2)
        return false;

    const ModelVolume *mv = wall_owning_volume(object, region);
    if (mv == nullptr)
        return false;

    for (const Vec3f &v : mv->mesh().its.vertices)
        ctx.mesh_box.merge(v.cast<double>());
    if (!ctx.mesh_box.defined)
        return false;

    ctx.mesh_from_print = (object.trafo_centered() * mv->get_matrix()).inverse();

    ctx.row = mf;
    // Nozzle-bounded resolution, the same rule phase 3 applies to top surfaces: a run shorter
    // than one extrusion width cannot be printed as its own colour, so image_fill_dither_segment
    // folds it into a neighbour. Using the SAME width for walls as for tops is what makes the
    // brief's "at the same sampling resolution as the top surfaces" literally true.
    const float w         = flow_width_mm > 0.05f ? flow_width_mm : 0.4f;
    ctx.min_run_len_mm    = w;
    ctx.sample_spacing_mm = w;
    // The first inner perimeter is claimed whenever the region actually has more than one wall
    // loop to give. A single-wall region has no inner perimeter, and claiming one that does not
    // exist would simply never match.
    ctx.split_first_inner = region.config().wall_loops.value > 1;
    return true;
}

} // namespace

bool image_row_wall_context_for_region(const PrintObject &object, const PrintRegion &region,
                                       float flow_width_mm, ImageRowWallContext &ctx)
{
    return build_context_for_row(object, region, image_row_wall_configured_virtual_id(object, region),
                                 flow_width_mm, ctx);
}

bool image_row_wall_iron_context(const PrintObject &object, const PrintRegion &region,
                                 float flow_width_mm, ImageRowWallContext &ctx)
{
    const Print *print = object.print();
    if (print == nullptr)
        return false;
    // Ironing follows solid_infill_filament (Layer::make_ironing), i.e. the SAME row phase 3's
    // top-surface split reads - see this function's declaration for why that is the point.
    const size_t       num_physical  = print->config().filament_diameter.size();
    const unsigned int configured_id = unsigned(std::max(0, region.config().solid_infill_filament.value));
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    if (!mixed_mgr.is_mixed(configured_id, num_physical))
        return false;
    const MixedFilament *mf = mixed_mgr.mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr || !mf->enabled || mf->distribution_mode != int(MixedFilament::ImageWeighted) ||
        mf->image_fill_ref.empty())
        return false;
    if (!build_context_for_row(object, region, configured_id, flow_width_mm, ctx))
        return false;
    // An ironed surface is a TOP surface: one plane, facing up. Hand the cutter that normal
    // instead of letting it compute a wall normal from the ironing line's own tangent, which
    // would make a Box projection pick a side face for a surface that faces up.
    const Vec3f up_mesh = (ctx.mesh_from_print.linear().cast<float>() * Vec3f(0.f, 0.f, 1.f));
    ctx.fixed_normal_mesh = up_mesh.norm() > 1e-9f ? Vec3f(up_mesh.normalized()) : Vec3f(0.f, 0.f, 1.f);
    return true;
}

std::vector<std::unique_ptr<ExtrusionEntityCollection>> image_row_split_wall_entity(
    const ImageAssetStore &assets, const ImageRowWallContext &ctx, const ExtrusionEntity &entity, double print_z)
{
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> out;

    ExtrusionPaths paths;
    if (!collect_wall_paths(entity, paths))
        return out;

    const Polyline whole = concatenated_polyline(paths);
    if (whole.points.size() < 2)
        return out;

    // A hole loop winds clockwise; see wall_outward_normal_print()'s comment for why the sign of
    // the normal matters (it is what a Box projection picks its face from, and what a cylindrical
    // projection's inward/outward face rule tests).
    bool ccw = true;
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        // ExtrusionLoop::is_counter_clockwise() is non-const; polygon() is, and is exactly
        // what that accessor delegates to (ExtrusionEntity.hpp:506).
        ccw = loop->polygon().is_counter_clockwise();

    // --- sample the whole wall, end to end, with a per-point wall normal -----------------------
    //
    // Unlike phase 3's top-surface sampler (which hands image_fill_project() ONE synthesised "up"
    // normal for the whole surface, because a top surface is one plane), a wall loop turns
    // corners: each segment of the loop faces a different direction, and on a box that means
    // different segments belong to different BOX FACES. So the normal is recomputed per segment
    // and sampled segment by segment, rather than once for the entity.
    std::vector<ImageRowSample> samples;
    float                       cumulative = 0.f;
    bool                        any_color  = false;
    const Points               &pts        = whole.points;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Vec3d p0_print(unscale<double>(pts[i].x()), unscale<double>(pts[i].y()), print_z);
        const Vec3d p1_print(unscale<double>(pts[i + 1].x()), unscale<double>(pts[i + 1].y()), print_z);
        const Vec3f p0_mesh = (ctx.mesh_from_print * p0_print).cast<float>();
        const Vec3f p1_mesh = (ctx.mesh_from_print * p1_print).cast<float>();

        // The wall normal, print space -> mesh space. Only the transform's LINEAR part applies
        // to a direction; the result is renormalised because a scaled transform does not preserve
        // length (and image_fill_box_face() compares components, so length is irrelevant to it -
        // but a zero-length normal would make it return a degenerate face).
        Vec3f n_mesh(0.f, 0.f, 1.f);
        if (ctx.fixed_normal_mesh.squaredNorm() > 1e-12f) {
            // A caller that already knows the surface's normal (a TOP surface, whose plane does
            // not turn along the path) supplies it once; see fixed_normal_mesh's own comment.
            n_mesh = ctx.fixed_normal_mesh;
        } else {
            const Vec3d n_print = wall_outward_normal_print(pts[i], pts[i + 1], ccw);
            if (n_print.squaredNorm() > 1e-18) {
                const Vec3d n_m = ctx.mesh_from_print.linear() * n_print;
                if (n_m.squaredNorm() > 1e-18)
                    n_mesh = n_m.normalized().cast<float>();
            }
        }

        std::vector<ImageRowSample> seg = image_fill_sample_segment(ctx.params, ctx.mesh_box, assets, p0_mesh, p1_mesh,
                                                                   n_mesh, ctx.sample_spacing_mm);
        // seg[0] repeats the previous segment's last sample (the shared corner point): skip it
        // past the first segment so a corner is sampled once, at one arc-length position, rather
        // than twice under two different running error-diffusion states.
        for (size_t k = (i == 0 ? 0 : 1); k < seg.size(); ++k) {
            ImageRowSample s = seg[k];
            s.s += cumulative;
            any_color = any_color || s.has_color;
            samples.push_back(s);
        }
        if (!seg.empty())
            cumulative += seg.back().s;
    }
    if (!any_color) {
        log_image_row_wall_no_color_once();
        return out;
    }

    const std::vector<ImageRowRun> runs =
        image_fill_dither_segment(samples, ctx.candidate_colors, ctx.candidate_ids, ctx.min_run_len_mm);
    if (runs.size() < 2)
        return out; // one colour for the whole wall: leave the loop intact, seam and all.

    // --- cut the wall at the run boundaries ---------------------------------------------------
    //
    // The wall is walked as a flat list of (segment, owning path) pairs so that every piece keeps
    // the attributes of the ExtrusionPath it came from - an outer loop's overhang sub-paths keep
    // printing as overhangs at overhang speed, which is why PerimeterGenerator retagged them in
    // the first place. Cutting the concatenated polyline alone would lose that.
    struct WallSeg
    {
        Point  a, b;
        size_t path_idx;   // index into `paths`, i.e. which attributes this segment prints with
        double len_mm;
    };
    std::vector<WallSeg> segs;
    for (size_t pi = 0; pi < paths.size(); ++pi) {
        const Points &pp = paths[pi].polyline.points;
        for (size_t k = 0; k + 1 < pp.size(); ++k) {
            const Vec2d  a(unscale<double>(pp[k].x()), unscale<double>(pp[k].y()));
            const Vec2d  b(unscale<double>(pp[k + 1].x()), unscale<double>(pp[k + 1].y()));
            const double len = (b - a).norm();
            if (len > EPSILON)
                segs.push_back(WallSeg{pp[k], pp[k + 1], pi, len});
        }
    }
    if (segs.empty())
        return out;

    // The cursor: which segment, and how far along it (mm) the previous run ended.
    size_t seg_idx = 0;
    double seg_off = 0.;

    auto point_at = [](const WallSeg &s, double off_mm) -> Point {
        const Vec2d a(unscale<double>(s.a.x()), unscale<double>(s.a.y()));
        const Vec2d b(unscale<double>(s.b.x()), unscale<double>(s.b.y()));
        const double t = s.len_mm > EPSILON ? std::min(1.0, std::max(0.0, off_mm / s.len_mm)) : 0.;
        const Vec2d  p = a + (b - a) * t;
        return Point(scale_(p.x()), scale_(p.y()));
    };

    // Takes `want_mm` of wall from the cursor, appending one ExtrusionPath per contiguous run of
    // segments that share a source path (so attributes never mix inside one emitted path).
    auto take = [&](double want_mm, ExtrusionPaths &dst) {
        Polyline cur;
        size_t   cur_src = size_t(-1);
        auto flush = [&]() {
            if (cur.points.size() >= 2 && cur_src < paths.size()) {
                ExtrusionPath ep(paths[cur_src]);
                ep.polyline = cur;
                dst.push_back(std::move(ep));
            }
            cur.points.clear();
        };
        while (want_mm > EPSILON && seg_idx < segs.size()) {
            const WallSeg &s     = segs[seg_idx];
            const double   avail = s.len_mm - seg_off;
            if (avail <= EPSILON) {
                ++seg_idx;
                seg_off = 0.;
                continue;
            }
            if (cur_src != s.path_idx) {
                flush();
                cur_src = s.path_idx;
            }
            if (cur.points.empty())
                cur.points.push_back(point_at(s, seg_off));
            if (avail <= want_mm + EPSILON) {
                cur.points.push_back(s.b);
                want_mm -= avail;
                ++seg_idx;
                seg_off = 0.;
            } else {
                seg_off += want_mm;
                cur.points.push_back(point_at(s, seg_off));
                want_mm = 0.;
            }
        }
        flush();
    };

    for (size_t r = 0; r < runs.size(); ++r) {
        ExtrusionPaths run_paths;
        // The last run takes everything left, so the pieces reconstruct the original wall exactly
        // rather than leaving a sliver behind from floating-point drift in run.s1.
        const double want = (r + 1 == runs.size()) ? std::numeric_limits<double>::max() / 4.
                                                   : std::max(0.0, double(runs[r].length()));
        take(want, run_paths);
        if (run_paths.empty())
            continue;

        auto coll = std::make_unique<ExtrusionEntityCollection>();
        // no_sort MUST stay false here, and the reason is not an optimisation - it is what makes
        // the run reach the nozzle at all. ObjectByExtruder::Island::Region::append() (GCode.cpp)
        // splats a sortable collection's children into the flat `perimeters` list, but pushes a
        // NON-sortable collection in WHOLE. GCode::extrude_perimeters() then hands each element
        // of that list to extrude_entity(), which accepts only ExtrusionPath / ExtrusionMultiPath
        // / ExtrusionLoop and throws InvalidArgument on a collection. Unlike fills - whose
        // emission path does descend into nested collections - perimeters have no such handling,
        // which is why phase 3's fill runs could set no_sort and these cannot.
        //
        // Nothing is lost by it: each run collection holds EXACTLY ONE entity (one ExtrusionPath
        // or one ExtrusionMultiPath covering that run's whole span), and a single entity has no
        // order to preserve. The ordering that does matter - runs printing in the sequence they
        // were cut - is not this flag's job anyway: it comes from the per-layer, per-extruder
        // grouping in GCode.cpp's emission loop, the same mechanism that bounds tool changes by
        // filament count rather than run count.
        coll->no_sort                   = false;
        coll->image_row_extruder_1based = unsigned(runs[r].filament_id);
        if (run_paths.size() == 1)
            coll->entities.push_back(new ExtrusionPath(run_paths.front()));
        else
            coll->entities.push_back(new ExtrusionMultiPath(run_paths));
        out.push_back(std::move(coll));
    }

    if (out.size() < 2)
        out.clear(); // nothing usefully split - let the caller keep the original loop.
    return out;
}

} // namespace Slic3r
