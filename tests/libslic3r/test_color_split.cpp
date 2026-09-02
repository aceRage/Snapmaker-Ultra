#include <catch2/catch.hpp>
#include <libslic3r/ColorSplit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <chrono>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Facet indices of its_make_cube(x, y, z) (same table as test_paint_depth_clamp.cpp:39-52):
// 0,1 = bottom (-Z); 2,3 = top (+Z); 4,5 = +X; 6,7 = +Y; 8,9 = -X; 10,11 = -Y.
static const std::vector<int> CUBE_TOP    = {2, 3};
static const std::vector<int> CUBE_BOTTOM = {0, 1};
static const std::vector<int> CUBE_PLUS_X = {4, 5};
static const std::vector<int> CUBE_SIDES  = {4, 5, 6, 7, 8, 9, 10, 11};

// Paint the given facets of `mesh` with `state` and return the serialized paint data,
// exactly what ModelVolume::mmu_segmentation_facets.get_data() would hold.
static TriangleSelector::TriangleSplittingData paint_data(const TriangleMesh &mesh,
                                                          const std::vector<std::pair<int, EnforcerBlockerType>> &facets)
{
    TriangleSelector selector(mesh);
    for (auto [facet, state] : facets)
        selector.set_facet(facet, state);
    return selector.serialize();
}

static std::vector<std::pair<int, EnforcerBlockerType>> all_with(const std::vector<int> &facets, EnforcerBlockerType st)
{
    std::vector<std::pair<int, EnforcerBlockerType>> out;
    for (int f : facets) out.emplace_back(f, st);
    return out;
}

// Paint every facet whose centroid/normal satisfies `pred` (for meshes whose facet order is not known,
// e.g. boolean results). `pred(centroid, normal)`.
template<class Pred>
static TriangleSelector::TriangleSplittingData paint_by_predicate(const TriangleMesh &mesh, Pred pred, EnforcerBlockerType st)
{
    TriangleSelector selector(mesh);
    const indexed_triangle_set &its = mesh.its;
    for (int f = 0; f < int(its.indices.size()); ++f) {
        const Vec3f a = its.vertices[its.indices[f][0]], b = its.vertices[its.indices[f][1]], c = its.vertices[its.indices[f][2]];
        const Vec3f n = (b - a).cross(c - a).normalized();
        if (pred((a + b + c) / 3.f, n))
            selector.set_facet(f, st);
    }
    return selector.serialize();
}

// A box x*y*z whose TOP face is an nx*ny grid (so paint can touch at a single vertex);
// vertices: bottom 4 corners then the (nx+1)*(ny+1) top grid; all faces CCW outward.
static TriangleMesh make_grid_box(double x, double y, double z, int nx, int ny)
{
    indexed_triangle_set its;
    auto V = [&](double px, double py, double pz) { its.vertices.emplace_back(float(px), float(py), float(pz)); return int(its.vertices.size()) - 1; };
    const int b0 = V(0, 0, 0), b1 = V(x, 0, 0), b2 = V(x, y, 0), b3 = V(0, y, 0);
    std::vector<int> top((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            top[j * (nx + 1) + i] = V(x * i / nx, y * j / ny, z);
    auto T = [&](int a, int b, int c) { its.indices.emplace_back(a, b, c); };
    T(b0, b2, b1); T(b0, b3, b2);                                   // bottom (-Z)
    for (int j = 0; j < ny; ++j)                                    // top grid (+Z)
        for (int i = 0; i < nx; ++i) {
            int p = top[j * (nx + 1) + i], q = top[j * (nx + 1) + i + 1], r = top[(j + 1) * (nx + 1) + i + 1], s = top[(j + 1) * (nx + 1) + i];
            T(p, q, r); T(p, r, s);
        }
    // sides: bottom edge -> top grid edge (fans against the grid's edge vertices)
    auto side = [&](int bA, int bB, const std::vector<int> &edge) {           // edge runs from above bA to above bB
        T(bA, bB, edge.front());
        for (size_t k = 0; k + 1 < edge.size(); ++k) T(bB, edge[k + 1], edge[k]);
    };
    std::vector<int> e_front, e_right, e_back, e_left;
    for (int i = 0; i <= nx; ++i) e_front.push_back(top[i]);
    for (int j = 0; j <= ny; ++j) e_right.push_back(top[j * (nx + 1) + nx]);
    for (int i = nx; i >= 0; --i) e_back.push_back(top[ny * (nx + 1) + i]);
    for (int j = ny; j >= 0; --j) e_left.push_back(top[j * (nx + 1)]);
    side(b0, b1, e_front); side(b1, b2, e_right); side(b2, b3, e_back); side(b3, b0, e_left);
    TriangleMesh mesh(std::move(its));
    REQUIRE(its_num_open_edges(mesh.its) == 0);
    return mesh;
}

TEST_CASE("colorsplit: strict patches share boundary vertices and cover the surface", "[colorsplit]")
{
    TriangleMesh cube = make_cube(40., 40., 20.);
    auto data = paint_data(cube, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorPatches p = extract_color_patches(cube.its, data);
    REQUIRE(p.states == std::vector<int>{2});
    REQUIRE(p.facet_state.size() == p.surface.indices.size());
    REQUIRE(its_num_open_edges(p.surface) == 0);
    REQUIRE_THAT(its_volume(p.surface), WithinRel(40. * 40. * 20., 1e-6));
    size_t painted = 0;
    for (int s : p.facet_state) painted += (s == 2);
    REQUIRE(painted == 2);
    size_t unpainted = 0;
    for (int s : p.facet_state) unpainted += (s == 0);
    REQUIRE(unpainted == p.surface.indices.size() - painted);
}

TEST_CASE("colorsplit: a brush stroke cutting through facets still yields a closed surface", "[colorsplit]")
{
    TriangleMesh cube = make_cube(40., 40., 20.);
    TriangleSelector selector(cube);
    // Paint a sphere-shaped patch through the middle of the top face (forces octree splitting / T-joints).
    // center on the top face, source = camera position above it (mesh coords), no clipping plane.
    selector.select_patch(2, std::make_unique<TriangleSelector::Sphere>(Vec3f(20.f, 20.f, 20.f), Vec3f(20.f, 20.f, 100.f), 6.f,
                                                                        Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder3, Transform3d::Identity(), true, 0.f);
    ColorPatches p = extract_color_patches(cube.its, selector.serialize());
    REQUIRE(p.states == std::vector<int>{3});
    REQUIRE(its_num_open_edges(p.surface) == 0);
    // its_volume (TriangleMesh.cpp) accumulates in float via a per-triangle normalize()/norm() (sqrt-based)
    // term; select_patch's octree subdivision around the sphere cursor's boundary adds dozens of extra,
    // non-axis-aligned triangles versus the 12-triangle base cube, so rounding accumulates to ~4e-6 relative
    // here (deterministic, verified by inspection: its_compactify_vertices does no coordinate arithmetic, and
    // the zero-open-edges check above already confirms the surface is topologically exact) - tighter than
    // epsilon(1e-6) can reliably clear. 1e-4 keeps a wide margin below the observed drift while still catching
    // a real defect (a missing/duplicated patch would be orders of magnitude larger). Matcher per tests/CLAUDE.md
    // (Approx is deprecated for this codebase; WithinRel is the documented replacement).
    REQUIRE_THAT(its_volume(p.surface), Catch::Matchers::WithinRel(40. * 40. * 20., 1e-4));
    REQUIRE(p.surface.indices.size() > cube.its.indices.size());
}

TEST_CASE("colorsplit: an open mesh is refused", "[colorsplit]")
{
    TriangleMesh cube = make_cube(10., 10., 10.);
    indexed_triangle_set open = cube.its;
    open.indices.pop_back();
    TriangleMesh open_mesh(open);
    auto data = paint_data(open_mesh, all_with({0}, EnforcerBlockerType::Extruder2));
    REQUIRE_THROWS_AS(extract_color_patches(open_mesh.its, data), ColorSplitError);
}

static DynamicPrintConfig split_test_config(PaintDepthMode mode = pdmWalls, int walls = 3, double mm = 1.5)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020"};
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionFloat>("layer_height")->value = 0.2;
    config.option<ConfigOptionInt>("top_shell_layers")->value = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value = 0.;
    config.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value = mode;
    config.option<ConfigOptionInt>("paint_depth_walls")->value = walls;
    config.option<ConfigOptionFloat>("paint_depth_mm")->value = mm;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = PerimeterGeneratorType::Classic;
    return config;
}

TEST_CASE("colorsplit: depths mirror paint_depth_band_mm and the shell layer rules", "[colorsplit]")
{
    DynamicPrintConfig cfg = split_test_config();
    ColorSplitDepths d = color_split_depths(cfg, {1, 2});
    Flow ext = Flow::new_from_config_width(frExternalPerimeter, *cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width"), 0.4f, 0.2f);
    Flow per = Flow::new_from_config_width(frPerimeter,         *cfg.option<ConfigOptionFloatOrPercent>("inner_wall_line_width"), 0.4f, 0.2f);
    float band = paint_depth_band_mm(pdmWalls, 3, 1.5, ext.width(), ext.spacing(), per.spacing());
    band = paint_depth_band_classic_floor_mm(band, ext.width(), ext.spacing());
    REQUIRE_THAT(d.D, WithinRel(double(band), 1e-5));
    REQUIRE_THAT(d.ws, WithinRel(double(ext.width() + ext.spacing()), 1e-5));
    REQUIRE_THAT(d.layer_height, WithinRel(0.2, 1e-5));
    REQUIRE(!d.unlimited);
    // top: max(4 layers, 0.6mm/0.2 = 3 layers) = 4 layers = 0.8mm; bottom: 3 layers, thickness 0 -> 0.6mm
    REQUIRE_THAT(d.cap_top, WithinRel(0.8, 1e-5));
    REQUIRE_THAT(d.cap_bottom, WithinRel(0.6, 1e-5));

    cfg.option<ConfigOptionInt>("top_shell_layers")->value = 0;          // zero count = no shell: surface layer only
    REQUIRE_THAT(color_split_depths(cfg, {1, 2}).cap_top, WithinRel(0.2, 1e-5));

    ColorSplitDepths u = color_split_depths(split_test_config(pdmUnlimited), {1, 2});
    REQUIRE(u.unlimited);
    REQUIRE_THAT(color_split_depths(split_test_config(pdmMillimeters, 3, 2.5), {1, 2}).D, WithinRel(2.5, 1e-5));
}

TEST_CASE("colorsplit: per-vertex depth is min(D, half thickness)", "[colorsplit]")
{
    // 40x40x1.2 plate: D = 1.5 must clamp on the top face. A plain make_cube only has CORNER vertices on
    // that face, and (as the block case below notes) their angle-weighted normal is the (+-1,+-1,+-1)/sqrt3
    // bisector, not vertical - so the -n ray runs diagonally through the plate rather than straight down:
    // it covers the 1.2mm of vertical drop over a path of 1.2*sqrt(3) ~= 2.07846mm before exiting the
    // bottom face, giving t/2 - 0.002 ~= 1.03723mm (still < D = 1.5, so still a real clamp, just not down
    // to half the nominal thickness - that number only applies where the normal is actually vertical).
    TriangleMesh plate = make_cube(40., 40., 1.2);
    ColorPatches pp = extract_color_patches(plate.its, paint_data(plate, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<Vec3f> np = color_split_normals(pp.surface);
    std::vector<float> dp = compute_vertex_depths(pp, np, 1.5);
    for (size_t v = 0; v < pp.surface.vertices.size(); ++v)
        if (pp.surface.vertices[v].z() > 1.0f) REQUIRE_THAT(dp[v], WithinAbs(1.03723f, 1e-3f));

    // An INTERIOR vertex of a subdivided flat top only touches top-face triangles, so its angle-weighted
    // normal is exactly vertical and the ray runs straight down: t = 1.2mm exactly, giving the textbook
    // half-thickness-minus-delta clamp 1.2/2 - 0.002 = 0.598mm (the delta is part of the rule, not noise).
    TriangleMesh plate_grid = make_grid_box(40., 40., 1.2, 2, 2);
    ColorPatches pg = extract_color_patches(plate_grid.its, paint_by_predicate(plate_grid, [](Vec3f, Vec3f n) { return n.z() > 0.5f; }, EnforcerBlockerType::Extruder2));
    std::vector<Vec3f> npg = color_split_normals(pg.surface);
    std::vector<float> dpg = compute_vertex_depths(pg, npg, 1.5);
    size_t center = 0;
    float  best   = std::numeric_limits<float>::max();
    for (size_t v = 0; v < pg.surface.vertices.size(); ++v) {
        float dist = (pg.surface.vertices[v] - Vec3f(20.f, 20.f, 1.2f)).squaredNorm();
        if (dist < best) { best = dist; center = v; }
    }
    REQUIRE(best < 1e-6f); // the (20,20,1.2) grid point must survive welding/compaction unmoved
    REQUIRE_THAT(dpg[center], WithinAbs(0.598f, 1e-3f));

    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches pb = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<Vec3f> nb = color_split_normals(pb.surface);
    std::vector<float> db = compute_vertex_depths(pb, nb, 1.5);
    // Corner normals are bisectors (angle weighted -> exact (±1,±1,1)/sqrt3); the ray along -n exits far away.
    for (size_t v = 0; v < pb.surface.vertices.size(); ++v)
        REQUIRE_THAT(db[v], WithinAbs(1.5f, 1e-4f));
    // Unlimited (D = inf) -> half thickness along the normal: 10mm at the top-face interior direction is not
    // sampled on a plain cube (only corner vertices exist), corners see the body diagonal/2.
    std::vector<float> du = compute_vertex_depths(pb, nb, std::numeric_limits<double>::infinity());
    for (float x : du) REQUIRE(x > 1.5f);
}
