// Split by painted colour: the public API, the depth model and the one-shot pipeline. The two heavy stages
// live next door - ColorSplitShell.cpp builds the shells, ColorSplitPartition.cpp runs the Manifold booleans.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "ColorSplit.hpp"
#include "ColorSplitInternal.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

ColorPatches extract_color_patches(const indexed_triangle_set &mesh_in, const TriangleSelector::TriangleSplittingData &paint)
{
    // Weld exact duplicates so index adjacency (what TriangleSelector uses for T-joint resolution) sees the
    // real topology. its_merge_vertices keeps face order, so the paint's facet indexing stays valid.
    indexed_triangle_set mesh = mesh_in;
    its_merge_vertices(mesh);
    if (its_num_open_edges(mesh) != 0)
        throw ColorSplitError("The part is not watertight; repair it before splitting by colour.");

    TriangleMesh tm(mesh);
    TriangleSelector sel(tm);
    sel.deserialize(paint, /*needs_reset=*/false);

    ColorPatches out;
    std::vector<int> states;
    for (size_t s = 1; s < paint.used_states.size(); ++s)
        if (paint.used_states[s] && sel.has_facets(EnforcerBlockerType(s)))
            states.push_back(int(s));
    states.push_back(0); // unpainted remainder, processed last

    // All get_facets_strict calls from ONE selector share the same vertex pool (TriangleSelector.cpp:1478-1502),
    // so indices from different states refer to the same coordinates and can simply be concatenated.
    bool first = true;
    for (int s : states) {
        indexed_triangle_set part = sel.get_facets_strict(EnforcerBlockerType(s));
        if (first) { out.surface.vertices = part.vertices; first = false; }
        if (part.vertices.size() != out.surface.vertices.size())
            throw ColorSplitError("Paint data does not share one vertex pool (internal error).");
        for (const Vec3i32 &t : part.indices) {
            out.surface.indices.push_back(t);
            out.facet_state.push_back(s);
        }
    }
    its_compactify_vertices(out.surface, /*shrink_to_fit=*/true);
    if (its_num_open_edges(out.surface) != 0)
        throw ColorSplitError("Paint data does not cover the surface consistently (internal error).");
    states.pop_back();
    out.states = std::move(states);
    return out;
}

static int effective_shell_layers(int n_layers, double thickness, double h)
{
    // Mirrors effective_shell_layers_by_thickness (MultiMaterialSegmentation.cpp:1381-1420) for uniform layers:
    // a zero count means no shell at all; otherwise the larger of the count and the layers spanning the thickness.
    if (n_layers <= 0) return 0;
    int by_thickness = thickness > 0. ? int(std::ceil(thickness / h - EPSILON)) : 0;
    return std::max(n_layers, by_thickness);
}

ColorSplitDepths color_split_depths(const DynamicPrintConfig &cfg, const std::vector<int> &filaments)
{
    if (filaments.empty())
        throw ColorSplitError("No filaments to derive the split depth from.");
    ColorSplitDepths out;
    const double h = cfg.opt_float("layer_height");
    out.layer_height = h;
    const PaintDepthMode mode  = cfg.opt_enum<PaintDepthMode>("paint_depth_mode");
    const int            walls = cfg.opt_int("paint_depth_walls");
    const double         mm    = cfg.opt_float("paint_depth_mm");
    const bool classic = cfg.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Classic;
    ConfigOptionFloatOrPercent ext_w = *cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    ConfigOptionFloatOrPercent per_w = *cfg.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
    if (ext_w.value == 0) ext_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");   // PrintRegion.cpp:95-96
    if (per_w.value == 0) per_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");
    const auto &nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values;
    if (nozzles.empty())
        throw ColorSplitError("Printer profile has no nozzle diameters.");
    out.unlimited = mode == pdmUnlimited;
    for (int f : filaments) {
        const float nozzle = float(nozzles[std::min<size_t>(std::max(f, 1) - 1, nozzles.size() - 1)]);
        Flow ext = Flow::new_from_config_width(frExternalPerimeter, ext_w, nozzle, float(h));
        Flow per = Flow::new_from_config_width(frPerimeter,         per_w, nozzle, float(h));
        float band = paint_depth_band_mm(mode, walls, mm, ext.width(), ext.spacing(), per.spacing());
        if (classic) band = paint_depth_band_classic_floor_mm(band, ext.width(), ext.spacing());
        out.D  = std::max(out.D,  double(band));
        out.ws = std::max(out.ws, double(ext.width() + ext.spacing()));
    }
    int n_top = effective_shell_layers(cfg.opt_int("top_shell_layers"),    cfg.opt_float("top_shell_thickness"),    h);
    int n_bot = effective_shell_layers(cfg.opt_int("bottom_shell_layers"), cfg.opt_float("bottom_shell_thickness"), h);
    out.cap_top    = std::max(h, n_top * h);
    out.cap_bottom = std::max(h, n_bot * h);
    return out;
}

std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface)
{
    // Spec 3.2: angle-weighted vertex normals - triangulation-independent, so a cube edge gets the exact
    // 45 degree bisector - always computed on the FULL surface F. NormalUtils::Normals is already
    // std::vector<Vec3f> (NormalUtils.hpp:17), so no copy is needed - but create_normals_angle_weighted
    // (NormalUtils.cpp:89-92) only divides by the summed angle weight, it does not renormalize to unit
    // length, so that step is still required here.
    std::vector<Vec3f> normals = NormalUtils::create_normals(surface, NormalUtils::VertexNormalType::AngleWeighted);
    for (Vec3f &n : normals) n.normalize();
    return normals;
}

namespace ColorSplitDetail {

// The dialog's depth override wins over both the depth AND the unlimited flag. Both entry points have to
// apply it identically - build_color_shells to cut with, split_volume_by_paint to report back - so neither
// gets to spell the rule out for itself.
ColorSplitDepths effective_depths(const ColorSplitDepths &depths, const ColorSplitParams &params)
{
    ColorSplitDepths out = depths;
    if (params.depth_override_mm > 0.) { out.D = params.depth_override_mm; out.unlimited = false; }
    return out;
}

float half_thickness_along(const AABBMesh &aabb, const Vec3f &v, const Vec3f &dir)
{
    const double eps = 1e-3;
    const Vec3d  n   = dir.cast<double>();
    const Vec3d  src = v.cast<double>() - eps * n;
    double t = std::numeric_limits<double>::infinity();
    // AABBMesh::query_ray_hits sorts hits by distance (AABBMesh.cpp:187-189), so the first hit past the
    // self-intersection radius is already the nearest one.
    for (const AABBMesh::hit_result &hit : aabb.query_ray_hits(src, -n))
        if (hit.is_hit() && hit.distance() > 5. * eps) { t = hit.distance() + eps; break; }
    // std::max(0., ...): on a feature thinner than 2 * delta the clamp t/2 - delta turns negative, which
    // would push the bottom copies OUTSIDE the part. The fold guard cannot catch that - where the offset
    // is parallel (one flat patch, one normal) a negative depth translates the bottom triangle without
    // changing its orientation, so the guard's orientation test still passes.
    return float(std::max(0., t / 2. - 0.002));
}

std::vector<float> vertex_depths(const AABBMesh &aabb, const ColorPatches &p, const std::vector<Vec3f> &normals, double D,
                                 std::vector<float> *half_thickness)
{
    const float cap = float(std::isfinite(D) ? D : std::numeric_limits<float>::max());
    std::vector<float> d(p.surface.vertices.size());
    if (half_thickness) half_thickness->resize(p.surface.vertices.size());
    for (size_t v = 0; v < p.surface.vertices.size(); ++v) {
        // One probe answers both questions, so spec 3.4a's clamp costs no extra ray casts: d(v) is this
        // number capped at D, and the mitred length below is bounded by the number itself.
        const float half = half_thickness_along(aabb, p.surface.vertices[v], normals[v]);
        if (half_thickness) (*half_thickness)[v] = half;
        d[v] = std::min(cap, half);
    }
    return d;
}

} // namespace ColorSplitDetail

std::vector<float> compute_vertex_depths(const ColorPatches &p, const std::vector<Vec3f> &normals, double D)
{
    return ColorSplitDetail::vertex_depths(AABBMesh(p.surface), p, normals, D);
}

ShellCheck check_shell(const indexed_triangle_set &shell)
{
    ShellCheck c;
    c.closed = its_num_open_edges(shell) == 0;
    c.volume = double(its_volume(shell));        // signed: positive for an outward-oriented closed mesh
    c.self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(shell));
    return c;
}

ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress,
                                       const Transform3d &to_split)
{
    if (progress && !progress(0)) throw ColorSplitCancelled();
    // Ruling 23: the paint is resolved in MESH space whatever space the split runs in, then the finished
    // surface travels. its_transform's left-handed fix swaps two vertices of every facet; doing that to the
    // RAW mesh would re-order the vertices the paint's sub-facet subdivision is expressed against and mirror
    // the stroke inside every partially painted facet. On the retriangulated surface the same swap only
    // reverses a facet's own winding - facet order, and so `facet_state`, is untouched.
    ColorPatches patches = extract_color_patches(mesh, paint);
    if (patches.states.empty()) throw ColorSplitError("The part has no painted colours.");
    if (!to_split.isApprox(Transform3d::Identity()))
        its_transform(patches.surface, to_split, /*fix_left_handed=*/true);
    if (progress && !progress(10)) throw ColorSplitCancelled();
    std::vector<std::string> shell_warnings;
    // Ruling 10: skipped micro-components are warnings, not errors - they have to reach the caller.
    std::vector<ColorShell> shells = build_color_shells(patches, depths, params, progress, &shell_warnings);
    ColorSplitResult r = partition_by_shells(patches.surface, shells, params.absorb_islands, progress);
    r.warnings.insert(r.warnings.begin(), shell_warnings.begin(), shell_warnings.end());
    r.depths = ColorSplitDetail::effective_depths(depths, params);
    if (progress) progress(100);
    return r;
}

// ---------------------------------------------------------------------------------------------------------
// Spec 3.9 / 4: coordinate space and the model mutation.

ColorSplitSpace color_split_space(const ModelObject &object, const ModelVolume &volume)
{
    ColorSplitSpace s;
    // The first instance stands for all of them: with an isotropic scale the mesh-space depths below are
    // exact for every instance sharing that scale, and an anisotropic one is a documented approximation
    // for the rest (spec 3.9).
    const Transform3d T = (object.instances.empty() ? Transform3d::Identity() : object.instances.front()->get_matrix()) * volume.get_matrix();
    const Matrix3d L  = T.linear();
    const double   sx = L.col(0).norm(), sy = L.col(1).norm(), sz = L.col(2).norm();
    // A flattened or otherwise singular transformation has neither a depth scale nor an inverse to bring the
    // pieces home with, so it is refused here rather than inverted.
    if (std::min({sx, sy, sz}) <= 1e-9 || std::abs(L.determinant()) <= 1e-9 * sx * sy * sz)
        throw ColorSplitError("Degenerate part transformation.");
    // Isotropic iff L^T L is s^2 I. Equal column norms alone are not enough: a rotated anisotropic scale
    // (R S R^T with R off-axis) can have three columns of the same length that are no longer orthogonal, and
    // Geometry::Transformation happily carries such a matrix - ModelObject::delete_volume builds one by
    // multiplying an instance matrix by a volume matrix. The off-diagonal test rejects exactly that, while
    // rotation and mirroring (which leave L^T L = s^2 I) stay on the cheap mesh-space path.
    const Matrix3d G   = L.transpose() * L;
    const double   tol = 1e-6 * sx * sx;
    if (std::abs(sx - sy) < 1e-6 * sx && std::abs(sx - sz) < 1e-6 * sx &&
        std::abs(G(0, 1)) < tol && std::abs(G(0, 2)) < tol && std::abs(G(1, 2)) < tol) {
        s.depth_scale = sx;
        return s;                                  // mesh-space path, identity transforms
    }
    s.world_path = true;
    s.to_split   = T;
    s.from_split = T.inverse();
    return s;
}

ColorSplitDepths scale_depths(const ColorSplitDepths &d, double s)
{
    ColorSplitDepths o = d;
    o.D /= s; o.ws /= s; o.cap_top /= s; o.cap_bottom /= s; o.layer_height /= s;
    return o;
}

ColorSplitParams scale_params(const ColorSplitParams &p, double s)
{
    ColorSplitParams o = p;
    // effective_depths applies the override AFTER the caller has scaled the depths, so on the mesh-space path
    // an unscaled override would cut at its world value. Zero and negative mean "no override" - leave them.
    if (o.depth_override_mm > 0.) o.depth_override_mm /= s;
    return o;
}

// One output volume of the split, in the source's own slot-to-be. `its` is in split space.
static ModelVolume *add_split_volume(ModelObject &object, const ModelVolume &src, indexed_triangle_set &&its,
                                     const ColorSplitSpace &space, const std::string &name)
{
    TriangleMesh mesh(std::move(its));
    // TriangleMesh::transform(t, fix_left_handed = true) flips the winding itself when the transform mirrors
    // (TriangleMesh.cpp:337-347); the caller carried the mesh out under the same flag, so the two flips cancel
    // and the piece comes back outward-facing.
    if (space.world_path)
        mesh.transform(space.from_split, /*fix_left_handed=*/true);
    // The public overload is the only way in: the (other, mesh&&) constructor is private. It copies the
    // source's name, config, type and transformation, leaves the facet annotations empty (asserted in the
    // constructor, Model.hpp:1162-1164) and centres the mesh.
    ModelVolume *v = object.add_volume(src, std::move(mesh));
    // Placement: centring translated the mesh by -shift and then added `shift` straight onto the offset
    // (ModelVolume::translate, Model.cpp:2901-2904), which is only right when the volume matrix has no
    // rotation or scale. The shift lives in MESH space, so it has to travel through the linear part.
    v->set_offset(src.get_offset() + src.get_transformation().get_matrix_no_offset() * v->mesh().get_init_shift());
    v->name = name;
    // The constructor copies these three; a split part is not a text or SVG volume any more (it must not be
    // regenerable from its glyphs) and it is not a cut connector either.
    v->text_configuration.reset();
    v->emboss_shape.reset();
    v->cut_info = ModelVolume::CutInfo();          // invalidate_cut_info() only clears is_connector
    v->source   = ModelVolume::Source();           // synthetic geometry: no reload from disk
    v->set_type(ModelVolumeType::MODEL_PART);
    return v;
}

std::vector<ModelVolume *> apply_color_split(ModelObject &object, size_t src_idx, ColorSplitResult &&r,
                                             const ColorSplitSpace &space, bool solid_interfaces, bool keep_base_sparse_infill)
{
    assert(src_idx < object.volumes.size() && object.volumes[src_idx]->is_model_part());
    ModelVolume      &src        = *object.volumes[src_idx];
    const std::string src_name   = src.name;
    const int         body_extruder = src.extruder_id();          // resolves volume -> object -> 0
    // An explicit zero means "inherit" just as much as a missing key does (ModelVolume::extruder_id).
    const bool        src_has_extruder = src.config.has("extruder") && src.config.extruder() != 0;
    const size_t      first_new  = object.volumes.size();
    std::vector<ModelVolume *> created;

    if (!r.body.indices.empty()) {
        ModelVolume *body = add_split_volume(object, src, std::move(r.body), space, src_name);
        // The body only carries an extruder of its own if the source did: otherwise it inherits the object's,
        // and the Ultra outer_wall_filament reset (PrintObject.cpp:3257-3259) is not re-triggered.
        if (src_has_extruder) body->config.set("extruder", body_extruder);
        else                  body->config.erase("extruder");
        created.push_back(body);
    }
    for (auto &piece : r.pieces) {
        if (piece.second.indices.empty())
            continue;    // dropped upstream with a warning; an empty volume would only clutter the list
        ModelVolume *part = add_split_volume(object, src, std::move(piece.second), space,
                                             src_name + " F" + std::to_string(piece.first));
        part->config.set("extruder", piece.first);
        // "Keep base-colour sparse infill": only the part's walls change colour, its interior stays the
        // body's filament - the 2D behaviour at PrintApply.cpp:1096-1103.
        if (keep_base_sparse_infill) part->config.set("sparse_infill_filament", std::max(1, body_extruder));
        created.push_back(part);
    }
    if (created.empty())
        return created;    // nothing to put in the source's place: leave the object exactly as it was

    // Move the new volumes from the back into the source's slot (body first, then the parts ascending) so the
    // modifiers and negatives behind it keep their relative order and the body-first slice-time clipping order
    // holds. delete_volume takes an index - it is the only removal API ModelObject offers.
    std::rotate(object.volumes.begin() + src_idx + 1, object.volumes.begin() + first_new, object.volumes.end());
    object.delete_volume(src_idx);
    // Explicitly, not through the paint: has_bounded_paint_depth() needs is_mm_painted() (Print.hpp:514) and
    // there is no paint left on the part now.
    if (solid_interfaces) object.config.set("interface_shells", true);
    object.invalidate_bounding_box();
    return created;
}

} // namespace Slic3r
