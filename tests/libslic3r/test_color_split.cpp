#include <catch2/catch.hpp>
#include <libslic3r/ColorSplit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

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
    // Correlate the state with the geometry rather than with itself: exactly the two facets sitting on the
    // painted top face (centroid z == 20) carry state 2, every other facet of the surface carries state 0.
    size_t painted = 0;
    for (size_t f = 0; f < p.surface.indices.size(); ++f) {
        const Vec3i32 &t = p.surface.indices[f];
        const float cz = (p.surface.vertices[t[0]].z() + p.surface.vertices[t[1]].z() + p.surface.vertices[t[2]].z()) / 3.f;
        const bool on_top = cz > 20.f - 1e-3f;
        REQUIRE(p.facet_state[f] == (on_top ? 2 : 0));
        painted += on_top;
    }
    REQUIRE(painted == 2);
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

TEST_CASE("colorsplit: depths refuse a config that cannot produce a depth", "[colorsplit]")
{
    DynamicPrintConfig cfg = split_test_config();
    REQUIRE_THROWS_AS(color_split_depths(cfg, {}), ColorSplitError);       // no filament -> D and ws would stay 0
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.clear();
    REQUIRE_THROWS_AS(color_split_depths(cfg, {1, 2}), ColorSplitError);   // no nozzle -> the index would underflow
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

TEST_CASE("colorsplit: NormalUtils AngleWeighted normals are the exact corner bisector on a cube", "[colorsplit]")
{
    // Regression pin for the NormalUtils::indice_angle indexing fix (it was reading vertices[i1]/vertices[i]
    // - local triangle-relative positions 0/1/2 - directly, instead of going through the triangle's own
    // indices vertices[indice[i1]]/vertices[indice[i]], so every triangle got the SAME fixed weight pair
    // regardless of its own shape). color_split_normals is spec 3.2's AngleWeighted consumer, and every corner
    // of an axis-aligned cube must get the exact (+-1,+-1,+-1)/sqrt3 bisector: three mutually perpendicular
    // faces meet there with equal 90 degree angle weight each, so the angle-weighted average is just the
    // unweighted average of the three face normals.
    TriangleMesh cube = make_cube(40., 40., 20.);
    std::vector<Vec3f> n = color_split_normals(cube.its);
    const Vec3f centre(20.f, 20.f, 10.f);
    const float s = 1.f / std::sqrt(3.f);
    for (size_t v = 0; v < cube.its.vertices.size(); ++v) {
        const Vec3f &p = cube.its.vertices[v];
        const Vec3f expected(p.x() > centre.x() ? s : -s, p.y() > centre.y() ? s : -s, p.z() > centre.z() ? s : -s);
        REQUIRE_THAT(n[v].x(), WithinAbs(expected.x(), 1e-5f));
        REQUIRE_THAT(n[v].y(), WithinAbs(expected.y(), 1e-5f));
        REQUIRE_THAT(n[v].z(), WithinAbs(expected.z(), 1e-5f));
    }
}

static ColorSplitDepths depths_for_test(double D, double h = 0.2, double ws = 0.87)
{
    ColorSplitDepths d; d.D = D; d.ws = ws; d.layer_height = h; d.cap_top = 0.8; d.cap_bottom = 0.6; d.unlimited = !std::isfinite(D);
    return d;
}
static ColorSplitParams no_cap_no_step() { ColorSplitParams p; p.flat_cap = false; p.crease_step = false; return p; }
static double volume_of(const indexed_triangle_set &its) { return double(its_volume(its)); }

TEST_CASE("colorsplit: shell of a painted top face is a closed slab of depth D", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    // The only vertices of the painted top face are the four cube corners, whose angle-weighted normals are the
    // (+-1,+-1,1)/sqrt(3) bisectors. d = 1.5 is measured ALONG that normal, so each corner moves 1.5/sqrt(3) =
    // 0.866mm in x, in y AND in z - the slab is the frustum between the 40x40 top and a 38.268x38.268 bottom
    // 0.866mm below it, NOT a 1.5mm-deep slab (the brief's 37*37*1.5 lower bound assumed a 1.5mm vertical drop
    // and is unreachable by construction). Square frustum: h/3 * (A_top + A_bottom + sqrt(A_top*A_bottom)).
    const double off     = 1.5 / std::sqrt(3.);
    const double side    = 40. - 2. * off;
    const double frustum = off / 3. * (1600. + side * side + 40. * side);   // = 1326.507 mm^3
    REQUIRE(c.volume < 40. * 40. * off);                                    // strictly inside the straight prism
    REQUIRE_THAT(c.volume, WithinRel(frustum, 1e-4));                       // its_volume accumulates in float
}

TEST_CASE("colorsplit: painted sphere smaller than D gets a valid thin shell (fold guard)", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    ColorPatches p = extract_color_patches(sphere.its, paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    REQUIRE(c.volume > 0.);
    REQUIRE(c.volume < 4. / 3. * PI);          // hollow shell (bottom = delta-ball at the centre, or thicker after the guard), less than the full ball
}

TEST_CASE("colorsplit: thin plate painted on both sides gives two shells meeting mid-thickness", "[colorsplit]")
{
    TriangleMesh plate = make_cube(40., 40., 1.2);
    auto data = paint_data(plate, {{2, EnforcerBlockerType::Extruder2}, {3, EnforcerBlockerType::Extruder2}, {0, EnforcerBlockerType::Extruder3}, {1, EnforcerBlockerType::Extruder3}});
    ColorPatches p = extract_color_patches(plate.its, data);
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 2);
    std::vector<int> states;
    for (const ColorShell &s : shells) states.push_back(s.state);
    std::sort(states.begin(), states.end());
    REQUIRE(states == std::vector<int>{2, 3});   // one shell per painted filament, whatever order they are built in
    for (const ColorShell &s : shells) {
        ShellCheck c = check_shell(s.mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
        REQUIRE_THAT(c.volume, WithinRel(40. * 40. * 0.6, 0.08));   // frustum-ish, ~0.6mm deep
    }
}

TEST_CASE("colorsplit: pinch boundary (two cells touching at one vertex) builds a closed shell", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 40., 10., 4, 4);
    // cells (1,1) and (2,2) share exactly one vertex; each cell = 2 triangles: index = 2 + 2*(j*4+i) (+1)
    auto cell = [](int i, int j) { int base = 2 + 2 * (j * 4 + i); return std::vector<int>{base, base + 1}; };
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    for (int f : cell(1, 1)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    for (int f : cell(2, 2)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(box.its, paint_data(box, facets));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 2);          // one shell per edge-connected component
    for (const ColorShell &s : shells) {
        ShellCheck c = check_shell(s.mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
        REQUIRE_THAT(c.volume, WithinRel(10. * 10. * 1.5, 0.02));
    }
}

TEST_CASE("colorsplit: concave groove painted across the crease still yields a valid shell", "[colorsplit]")
{
    // L bracket: 40x40x20 block minus a 40x20x10 notch on the +Y/+Z corner, made with Manifold union of two boxes.
    TriangleMesh a = make_cube(40., 40., 10.);
    TriangleMesh b = make_cube(40., 20., 20.);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);
    TriangleMesh bracket = out.front();
    // paint the floor of the notch (z=10, y in 20..40) and the riser wall (y=20, z in 10..20)
    auto data = paint_by_predicate(bracket, [](const Vec3f &c, const Vec3f &n) {
        return (std::abs(n.z() - 1.f) < 1e-3f && c.z() > 9.9f && c.z() < 10.1f && c.y() > 20.f) || (std::abs(n.y() - 1.f) < 1e-3f && c.z() > 10.f);
    }, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(bracket.its, data);
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    // Spec 3.1a (Ruling 18): floor and riser meet at 90 degrees, so they are two smooth patches and two
    // shells whose claims overlap in the corner - which is exactly what lets each of them keep a straight
    // wall at their shared same-state crease instead of tapering away from it.
    REQUIRE(shells.size() == 2);
    for (const ColorShell &sh : shells) {
        REQUIRE(sh.state == 2);
        ShellCheck c = check_shell(sh.mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
    }
}

TEST_CASE("colorsplit: a self-touching patch builds one valid shell across its pinch vertex", "[colorsplit]")
{
    // Cells (2,2) and (3,3) are diagonal neighbours; the path (2,2)-(2,1)-(3,1)-(4,1)-(4,2)-(4,3)-(3,3) joins
    // them without ever using (3,2) or (2,3), so the painted region is ONE edge-connected component that comes
    // back and touches itself at grid vertex (3,3). That vertex carries four boundary edges, so the shell
    // splits it into two wedges - the case spec 3.7's per-wedge duplication exists for, and the case the
    // "two cells touching at one vertex" fixture above does NOT reach (there the cells are two components,
    // each with an ordinary two-edge boundary at the shared vertex).
    // The grid is 6x6 rather than 4x4 so that every painted cell stays off the box's top rim: rim vertices
    // carry 45 degree bisector normals, which bevel the shell and would cost ~17% of its volume, whereas an
    // all-interior patch has vertical normals and depth min(1.5, 10/2 - 0.002) = 1.5 everywhere, making the
    // shell an exact prism of area * 1.5.
    const int    n    = 6;
    const double side = 40. / n;
    TriangleMesh box  = make_grid_box(40., 40., 10., n, n);
    const std::vector<std::pair<int, int>> path{{2, 2}, {2, 1}, {3, 1}, {4, 1}, {4, 2}, {4, 3}, {3, 3}};
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    for (const auto &ij : path) {
        const int base = 2 + 2 * (ij.second * n + ij.first);   // cell (i,j) = triangles base, base+1
        facets.emplace_back(base,     EnforcerBlockerType::Extruder2);
        facets.emplace_back(base + 1, EnforcerBlockerType::Extruder2);
    }
    ColorPatches p = extract_color_patches(box.its, paint_data(box, facets));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);                                       // the pinch nudge keeps the wedges apart
    REQUIRE_THAT(c.volume, WithinRel(7. * side * side * 1.5, 0.02));
}

TEST_CASE("colorsplit: a shell on a 0.3mm plate stops short of mid-thickness", "[colorsplit]")
{
    // The mid-thickness clamp (and the halving floor that must not exceed it) keeps every shell vertex on the
    // painted side of the mid-surface, however thin the part is. The only vertices of a plain cube's top face
    // are its four corners, whose angle-weighted normals are the (+-1,+-1,1)/sqrt(3) bisectors, so the -n ray
    // crosses 0.3*sqrt(3) = 0.51962mm of plate and d0 = t/2 - 0.002 = 0.25781mm ALONG THE NORMAL - 0.148 is
    // the vertical-normal figure, which a plain make_cube never samples (the same distinction the per-vertex
    // depth test above documents). 0.25781mm along the bisector is 0.14885mm of vertical drop, so the bottom
    // copies land at z = 0.15115, just clear of the z = 0.15 mid-plane.
    TriangleMesh plate = make_cube(40., 40., 0.3);
    ColorPatches p = extract_color_patches(plate.its, paint_data(plate, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    const float drop = float(0.3 * std::sqrt(3.) / 2. - 0.002) / std::sqrt(3.f);   // d0 projected onto z
    for (const Vec3f &v : shells[0].mesh.vertices) {
        REQUIRE(v.z() > 0.15f);                       // never past mid-thickness, top or bottom copy
        REQUIRE(0.3f - v.z() <= drop + 1e-4f);        // and never deeper than the vertex's own d0
    }
}

TEST_CASE("colorsplit: a feature too small to carry a shell is skipped with a warning", "[colorsplit]")
{
    // A 0.3mm ball. d(v) = t/2 - 0.002 = 0.148mm everywhere, which is already below the layer height, so the
    // halving floor min(h, d0) cannot lower it any further - and at that depth every bottom copy lands within
    // 2 microns of the centre, where the offset triangles fold through one another. Spec 7 (rev 2.3): a feature
    // too small to carry a shell is NOT an error. The component is dropped, the body keeps that feature in its
    // own colour, and the user gets one note naming the filament and the feature's size.
    TriangleMesh sphere(its_make_sphere(0.15, PI / 18.));
    ColorPatches p = extract_color_patches(sphere.its, paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2));
    std::vector<std::string> warnings;
    std::vector<ColorShell>  shells;
    REQUIRE_NOTHROW(shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), no_cap_no_step(), nullptr, &warnings));
    REQUIRE(shells.empty());
    REQUIRE(warnings.size() == 1);
    REQUIRE(warnings[0].find("Filament 2") != std::string::npos);
    // The size quoted is the diagonal of the painted feature's own bounding box, computed here from the source
    // mesh (extract_color_patches only welds and compactifies, so the coordinates are the same ones).
    Vec3f lo = sphere.its.vertices.front(), hi = lo;
    for (const Vec3f &v : sphere.its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    std::ostringstream expected;
    expected << "Filament 2: a painted feature about " << std::fixed << std::setprecision(2) << (hi - lo).norm()
             << " mm across is too small to split and stays in the body colour.";
    REQUIRE(warnings[0] == expected.str());
}

TEST_CASE("colorsplit: the halving floor is what decides skip versus salvage", "[colorsplit]")
{
    // The same 0.15 mm ball as the skip test above; only the layer height differs. The fold guard and the
    // validity fallback halve towards the SAME floor, min(layer_height, d0) (Ruling 9). At h = 0.2 that floor
    // already equals d0 = 0.147, so neither can move anything and the feature is skipped with a warning. At
    // h = 0.02 the guard can act: it lowers the depth until the bottom copies stop folding through one another
    // near the centre and the shell is valid on its FIRST check - silently, because reining a convex feature's
    // offset in is ordinary work, not an event worth telling the user about. This is also why no fixture in
    // this suite reaches the fallback's "shell depth reduced" note (spec 9 / spike-report.md): wherever the
    // guard can act it gets there first, and where it cannot the fallback cannot either.
    TriangleMesh sphere(its_make_sphere(0.15, PI / 18.));
    ColorPatches p = extract_color_patches(sphere.its, paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2));
    std::vector<std::string> warnings;
    std::vector<ColorShell>  shells = build_color_shells(p, depths_for_test(1.5, 0.02), no_cap_no_step(), nullptr, &warnings);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    REQUIRE(c.volume > 0.);
    REQUIRE(warnings.empty());
}

TEST_CASE("colorsplit: progress stays inside the 0..100 contract with many components", "[colorsplit]")
{
    // 18 cells painted in a checkerboard: they only ever touch diagonally, so every one of them is its own
    // edge-connected component of a single state. ColorSplit.hpp documents the callback as a 0..100 percentage,
    // and this stage of the split owns the 10..50 band - the reported value has to respect that however many
    // components a state has, and painted text or a logo routinely produces dozens.
    const int n = 6;
    TriangleMesh box = make_grid_box(40., 40., 10., n, n);
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    int cells = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            if ((i + j) % 2 == 0) {
                const int base = 2 + 2 * (j * n + i);
                facets.emplace_back(base,     EnforcerBlockerType::Extruder2);
                facets.emplace_back(base + 1, EnforcerBlockerType::Extruder2);
                ++cells;
            }
    REQUIRE(cells == 18);
    ColorPatches p = extract_color_patches(box.its, paint_data(box, facets));
    std::vector<int> reported;
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(),
                                     [&reported](int percent) { reported.push_back(percent); return true; });
    REQUIRE(shells.size() == size_t(cells));
    REQUIRE(reported.size() == size_t(cells));       // one tick per component
    for (int percent : reported) {
        REQUIRE(percent >= 0);
        REQUIRE(percent <= 100);
    }
    REQUIRE(reported.back() <= 50);                  // and the band this stage owns
}

TEST_CASE("colorsplit: depth_override_mm replaces D and clears unlimited", "[colorsplit]")
{
    // Unlimited would take every vertex to half the block's thickness; the override has to win over both the
    // depth AND the unlimited flag. The painted top face's only vertices are the cube corners, whose
    // (+-1,+-1,1)/sqrt(3) bisector normals turn an 0.7mm depth into 0.7/sqrt(3) = 0.40415mm of vertical drop.
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = no_cap_no_step();
    params.depth_override_mm = 0.7;
    auto shells = build_color_shells(p, depths_for_test(std::numeric_limits<double>::infinity()), params, nullptr);
    REQUIRE(shells.size() == 1);
    float min_z = shells[0].mesh.vertices.front().z(), max_z = min_z;
    for (const Vec3f &v : shells[0].mesh.vertices) { min_z = std::min(min_z, v.z()); max_z = std::max(max_z, v.z()); }
    REQUIRE_THAT(max_z, WithinAbs(20.f, 1e-4f));
    REQUIRE_THAT(min_z, WithinAbs(20.f - float(0.7 / std::sqrt(3.)), 1e-4f));
}

TEST_CASE("colorsplit: partition of a painted top is exact and complementary", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorSplitResult r = split_volume_by_paint(block.its, data, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.pieces[0].first == 2);
    REQUIRE(its_num_open_edges(r.body) == 0);
    REQUIRE(its_num_open_edges(r.pieces[0].second) == 0);
    const double total = volume_of(r.body) + volume_of(r.pieces[0].second);
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
    REQUIRE(volume_of(r.pieces[0].second) < 40. * 40. * 1.5 + 1.);
    // The piece's top surface is the original top face: every piece vertex has z <= 20 and the max is 20.
    float zmax = -1.f; for (const Vec3f &v : r.pieces[0].second.vertices) zmax = std::max(zmax, v.z());
    REQUIRE_THAT(zmax, WithinRel(20.f, 1e-5f));
}

TEST_CASE("colorsplit: three adjacent colours tile the top face, lower filament wins overlaps", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 40., 20., 4, 1);   // top = 4 cells in a row
    auto cell = [](int i) { int base = 2 + 2 * i; return std::vector<int>{base, base + 1}; };
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    for (int f : cell(0)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    for (int f : cell(1)) facets.emplace_back(f, EnforcerBlockerType::Extruder3);
    for (int f : cell(2)) facets.emplace_back(f, EnforcerBlockerType::Extruder4);
    ColorSplitResult r = split_volume_by_paint(box.its, paint_data(box, facets), depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 3);
    double total = volume_of(r.body);
    for (auto &pc : r.pieces) { REQUIRE(its_num_open_edges(pc.second) == 0); total += volume_of(pc.second); }
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
    // pairwise disjoint: intersect pieces with Manifold and expect empty
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = i + 1; j < 3; ++j) {
            std::vector<TriangleMesh> out;
            REQUIRE(MeshBoolean::mfd::make_boolean(TriangleMesh(r.pieces[i].second), TriangleMesh(r.pieces[j].second), out, "INTERSECTION"));
            double v = 0.; for (auto &m : out) v += volume_of(m.its);
            REQUIRE(v < 1e-3);
        }
}

TEST_CASE("colorsplit: a filament left with no solid of its own is reported", "[colorsplit]")
{
    // Spec 3.8/7: overlaps settle by filament order, so a colour can lose ALL of its area to a lower one -
    // and then it never reaches the pieces at all, because the second Split finds nothing left to cut. Two
    // identical shells, filaments 2 and 3: filament 2 takes the whole slab and filament 3 must come back as a
    // warning rather than silently disappearing.
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<ColorShell> shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    shells.push_back({3, shells[0].capped, shells[0].mesh});   // the very same solid, one filament higher
    ColorSplitResult r = partition_by_shells(p.surface, shells, /*absorb_islands=*/true, nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.pieces[0].first == 2);
    REQUIRE(r.warnings.size() == 1);
    REQUIRE(r.warnings[0] == "Filament 3: painted area produced no solid (fully covered by lower filaments).");
}

TEST_CASE("colorsplit: the island where two colours meet is absorbed, not left in the body", "[colorsplit]")
{
    // Two hemispheres in different colours at UNLIMITED depth: each shell stops delta short of the
    // mid-surface, so what survives in the middle is one tiny island enclosed by both of them. Spec 3.8 hands
    // it to the colour that contributed most of its surface (ties -> the lower filament); either way it has
    // to end up inside a piece rather than as a stray speck of body colour in the centre of the part.
    TriangleMesh sphere(its_make_sphere(2.0, PI / 18.));
    TriangleSelector sel(sphere);
    for (int f = 0; f < int(sphere.its.indices.size()); ++f) {
        const Vec3i32 &t = sphere.its.indices[f];
        const float cz = (sphere.its.vertices[t[0]].z() + sphere.its.vertices[t[1]].z() + sphere.its.vertices[t[2]].z()) / 3.f;
        sel.set_facet(f, cz > 0.f ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::Extruder3);
    }
    ColorSplitResult r = split_volume_by_paint(sphere.its, sel.serialize(),
                                               depths_for_test(std::numeric_limits<double>::infinity()), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 2);
    REQUIRE(r.pieces[0].first == 2);
    REQUIRE(r.pieces[1].first == 3);
    REQUIRE(volume_of(r.body) < 1e-6);                       // absorbed into one of the two, whichever won
    double total = volume_of(r.body);
    for (auto &pc : r.pieces) total += volume_of(pc.second);
    REQUIRE_THAT(total, WithinRel(volume_of(sphere.its), 1e-4));
}

TEST_CASE("colorsplit: painted sphere smaller than D is wholly its colour (island absorption)", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    auto data = paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2);
    ColorSplitParams params = no_cap_no_step();
    ColorSplitResult r = split_volume_by_paint(sphere.its, data, depths_for_test(1.5), params, nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.body.indices.empty());
    REQUIRE_THAT(volume_of(r.pieces[0].second), WithinRel(volume_of(sphere.its), 1e-4));
    params.absorb_islands = false;
    ColorSplitResult r2 = split_volume_by_paint(sphere.its, data, depths_for_test(1.5), params, nullptr);
    REQUIRE(!r2.body.indices.empty());
}

TEST_CASE("colorsplit: cancellation aborts without a result", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    REQUIRE_THROWS_AS(split_volume_by_paint(block.its, data, depths_for_test(1.5), no_cap_no_step(), [](int) { return false; }), ColorSplitCancelled);
    // And once the partition itself is under way: build_color_shells owns the 10..50 band and
    // partition_by_shells 50..95, so a callback that only refuses above 50 cancels inside the Split loop.
    REQUIRE_THROWS_AS(split_volume_by_paint(block.its, data, depths_for_test(1.5), no_cap_no_step(), [](int percent) { return percent <= 50; }), ColorSplitCancelled);
}

TEST_CASE("colorsplit: an ultra-thin plate never asks for a negative depth", "[colorsplit]")
{
    // Task 3 re-review follow-up: d(v) = t(v)/2 - delta would turn NEGATIVE on a feature thinner than
    // 2 * delta = 0.004 mm, and the fold guard cannot catch that - a uniformly negative offset moves every
    // bottom copy OUTSIDE the part while preserving each triangle's orientation, so the guard's orientation
    // test still passes. compute_vertex_depths therefore clamps to >= 0.
    //
    // MEASURED on this 3 micron plate: every depth comes back as D (1.5), not as a small or negative number.
    // The thickness probe (ColorSplit.cpp) starts 0.001 mm inside the surface and ignores any hit closer than
    // 5 * 0.001 mm as a self-intersection, so a corner ray that crosses only 0.003 * sqrt(3) = 0.0052 mm of
    // plate and exits 0.0042 mm from the probe origin is discarded: t stays infinite and the depth falls back
    // to D. Any hit the probe DOES accept is > 0.005 mm away, i.e. t > 0.006 and t/2 - delta > 0.001, so the
    // clamp is unreachable through this path and is a guard, not a live branch. What the split must still do
    // is stay sane: the shell overshoots the plate by 0.86 mm, the partition clips it back, and the whole
    // plate comes out as the painted filament instead of yielding invalid geometry.
    TriangleMesh plate = make_cube(40., 40., 0.003);
    auto data = paint_data(plate, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorPatches p = extract_color_patches(plate.its, data);
    std::vector<Vec3f> n = color_split_normals(p.surface);
    std::vector<float> d = compute_vertex_depths(p, n, 1.5);
    REQUIRE(*std::min_element(d.begin(), d.end()) >= 0.f);
    REQUIRE_THAT(*std::max_element(d.begin(), d.end()), WithinAbs(1.5f, 1e-5f));
    ColorSplitResult r = split_volume_by_paint(plate.its, data, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.pieces[0].first == 2);
    REQUIRE(its_num_open_edges(r.pieces[0].second) == 0);
    REQUIRE(volume_of(r.pieces[0].second) > 0.99 * 40. * 40. * 0.003);
}

TEST_CASE("colorsplit: a painted boss on a block is entirely its colour (smooth-patch decomposition)", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 10.);
    TriangleMesh boss(its_make_cylinder(1.0, 4.0, PI / 18.));
    boss.translate(20.f, 20.f, 9.f);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(block, boss, out, "UNION"));
    TriangleMesh bossed = out.front();
    auto paint = paint_by_predicate(bossed, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.05f; }, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(bossed.its, paint);
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), ColorSplitParams{}, nullptr);
    REQUIRE(shells.size() == 2);                                       // side tube + top slab, both state 2
    for (const ColorShell &sh : shells) { REQUIRE(sh.state == 2); REQUIRE(check_shell(sh.mesh).closed); REQUIRE(!check_shell(sh.mesh).self_intersects); }
    ColorSplitResult rb = split_volume_by_paint(bossed.its, paint, depths_for_test(1.5, 0.2, 0.87), ColorSplitParams{}, nullptr);
    REQUIRE(rb.pieces.size() == 1);
    const double exposed = PI * 1.0 * 1.0 * 3.0;                        // 1 mm of the cylinder is buried in the block
    REQUIRE(volume_of(rb.pieces[0].second) >= 0.95 * exposed);
    float zmin = 1e9f; for (const Vec3f &v : rb.pieces[0].second.vertices) zmin = std::min(zmin, v.z());
    REQUIRE(zmin >= 10.f - 1e-3f);                                       // Ruling 14: nothing painted below the block top
    WARN("S1 boss piece " << volume_of(rb.pieces[0].second) << " mm^3 of " << exposed << " exposed ("
         << 100. * volume_of(rb.pieces[0].second) / exposed << " %)");
}

TEST_CASE("colorsplit: a painted cube top and side are two smooth patches with straight walls", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    std::vector<std::pair<int, EnforcerBlockerType>> facets = all_with(CUBE_TOP, EnforcerBlockerType::Extruder2);
    for (int f : CUBE_PLUS_X) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, facets));
    ColorSplitParams params; params.flat_cap = false; params.crease_step = false;
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(shells.size() == 2);
    // the top slab's bottom lies exactly 1.5 below the top at every vertex (straight walls at the same-state crease),
    // and the side slab's bottom exactly 1.5 inside the +X face
    for (const ColorShell &sh : shells) {
        float zmin = 1e9f, xmin = 1e9f;
        for (const Vec3f &v : sh.mesh.vertices) { zmin = std::min(zmin, v.z()); xmin = std::min(xmin, v.x()); }
        REQUIRE(((std::abs(zmin - 18.5f) < 1e-4f) || (std::abs(xmin - 38.5f) < 1e-4f)));
    }
    ColorSplitResult r = split_volume_by_paint(block.its, paint_data(block, facets), depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(r.pieces.size() == 1);
    double total = volume_of(r.body) + volume_of(r.pieces[0].second);
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
}

// ---- Spike measurements (spec 9). They must pass; numbers go to WARN for the decision checkpoint. ----

// its_make_sphere(r, fa) builds sectorCount = ceil(2*PI/fa) sectors over stackCount = ceil(PI/fa) stacks, i.e.
// sectorCount * (2 * stackCount - 2) triangles. The PI/90 of the task brief yields only 180 * 178 = 32040 -
// a third of the 100k the spike calls for - so this uses PI/158: 316 * 314 = 99224, inside the 80k..120k band.
static constexpr double SPIKE_SPHERE_FA = PI / 158.;

TEST_CASE("colorsplit spike: S1 self-intersecting fixtures and S3 timing", "[colorsplit_spike]")
{
    using clock = std::chrono::steady_clock;
    // S3: ~100k-triangle sphere, one colour cap and three colour bands.
    TriangleMesh sphere(its_make_sphere(20.0, SPIKE_SPHERE_FA));
    WARN("sphere triangles: " << sphere.its.indices.size());
    auto one = paint_by_predicate(sphere, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.f; }, EnforcerBlockerType::Extruder2);
    auto t0 = clock::now();
    ColorSplitResult r1 = split_volume_by_paint(sphere.its, one, depths_for_test(1.5), ColorSplitParams{}, nullptr);
    auto t1 = clock::now();
    WARN("S3 one colour: " << std::chrono::duration<double>(t1 - t0).count() << " s, pieces " << r1.pieces.size()
                           << ", warnings " << r1.warnings.size());
    for (const std::string &w : r1.warnings) WARN("S3 one colour warning: " << w);
    TriangleSelector sel(sphere);
    for (int f = 0; f < int(sphere.its.indices.size()); ++f) {
        const auto &t = sphere.its.indices[f];
        float z = (sphere.its.vertices[t[0]].z() + sphere.its.vertices[t[1]].z() + sphere.its.vertices[t[2]].z()) / 3.f;
        if (z > 10.f) sel.set_facet(f, EnforcerBlockerType::Extruder2);
        else if (z > -5.f) sel.set_facet(f, EnforcerBlockerType::Extruder3);
        else if (z > -15.f) sel.set_facet(f, EnforcerBlockerType::Extruder4);
    }
    t0 = clock::now();
    ColorSplitResult r3 = split_volume_by_paint(sphere.its, sel.serialize(), depths_for_test(1.5), ColorSplitParams{}, nullptr);
    t1 = clock::now();
    WARN("S3 three colours: " << std::chrono::duration<double>(t1 - t0).count() << " s, pieces " << r3.pieces.size()
                              << ", warnings " << r3.warnings.size());
    for (const std::string &w : r3.warnings) WARN("S3 three colours warning: " << w);
    double total = volume_of(r3.body); for (auto &pc : r3.pieces) total += volume_of(pc.second);
    REQUIRE_THAT(total, WithinRel(volume_of(sphere.its), 1e-4));

    // S1: 2mm boss on a block (convex feature narrower than 2D) painted entirely.
    TriangleMesh block = make_cube(40., 40., 10.);
    TriangleMesh boss(its_make_cylinder(1.0, 4.0, PI / 18.));
    boss.translate(20.f, 20.f, 9.f);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(block, boss, out, "UNION"));
    TriangleMesh bossed = out.front();
    auto paint = paint_by_predicate(bossed, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.05f; }, EnforcerBlockerType::Extruder2);
    // S1 asks for the shells' own volumes as well as the partition's, so the two stages run separately here
    // (this is exactly what split_volume_by_paint chains internally).
    ColorPatches pb = extract_color_patches(bossed.its, paint);
    std::vector<std::string> bw;
    std::vector<ColorShell>  bs = build_color_shells(pb, depths_for_test(1.5), ColorSplitParams{}, nullptr, &bw);
    for (const ColorShell &sh : bs) WARN("S1 boss shell: filament " << sh.state << ", volume " << check_shell(sh.mesh).volume);
    for (const std::string &w : bw) WARN("S1 boss warning: " << w);
    REQUIRE(bs.size() == 2);                    // spec 3.1a: the boss side and its top cap are two patches
    ColorSplitResult rb = partition_by_shells(pb.surface, bs, /*absorb_islands=*/true, nullptr);
    REQUIRE(rb.pieces.size() == 1);
    WARN("S1 boss: piece volume " << volume_of(rb.pieces[0].second) << ", body volume " << volume_of(rb.body)
         << " (whole boss " << PI * 4.0 << ", boss above the block " << PI * 3.0 << ")");
    // Everything this fixture now pins - the piece stays above the block top, reaches the boss top and spans
    // its full diameter - lives in the "[colorsplit]" boss test; here the numbers are the spike measurement.
}

TEST_CASE("colorsplit spike: S3 stage breakdown and the CGAL check share", "[colorsplit_spike]")
{
    // Same ~100k sphere, one colour, run stage by stage so the CGAL self-intersection check can be timed on
    // its own: build_color_shells calls check_shell once per component when no fallback round is needed, so
    // re-checking each finished shell here measures exactly the share of that stage that is spent in CGAL.
    using clock = std::chrono::steady_clock;
    TriangleMesh sphere(its_make_sphere(20.0, SPIKE_SPHERE_FA));
    auto paint = paint_by_predicate(sphere, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.f; }, EnforcerBlockerType::Extruder2);

    auto t0 = clock::now();
    ColorPatches p = extract_color_patches(sphere.its, paint);
    auto t1 = clock::now();
    std::vector<std::string> warnings;
    std::vector<ColorShell> shells = build_color_shells(p, depths_for_test(1.5), ColorSplitParams{}, nullptr, &warnings);
    auto t2 = clock::now();
    REQUIRE(shells.size() == 1);
    double check_s = 0.;
    for (const ColorShell &s : shells) {
        auto c0 = clock::now();
        ShellCheck c = check_shell(s.mesh);
        check_s += std::chrono::duration<double>(clock::now() - c0).count();
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
    }
    auto t3 = clock::now();
    ColorSplitResult r = partition_by_shells(p.surface, shells, /*absorb_islands=*/true, nullptr);
    auto t4 = clock::now();
    REQUIRE(r.pieces.size() == 1);

    const double patches_s   = std::chrono::duration<double>(t1 - t0).count();
    const double shells_s    = std::chrono::duration<double>(t2 - t1).count();
    const double partition_s = std::chrono::duration<double>(t4 - t3).count();
    WARN("S3 breakdown (" << sphere.its.indices.size() << " tri, shell " << shells[0].mesh.indices.size() << " tri): patches "
         << patches_s << " s, shells " << shells_s << " s (of which check_shell/CGAL " << check_s << " s = "
         << (shells_s > 0. ? 100. * check_s / shells_s : 0.) << "% of that stage, "
         << (100. * check_s / std::max(1e-9, patches_s + shells_s + partition_s)) << "% of the whole split), partition "
         << partition_s << " s; shell warnings " << warnings.size());
    for (const std::string &w : warnings) WARN("S3 breakdown warning: " << w);
}
