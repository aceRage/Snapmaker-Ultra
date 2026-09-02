#include <catch2/catch.hpp>
#include <libslic3r/ColorSplit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Layer.hpp>
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

// Facet indices of its_make_cube(x, y, z) (TriangleMesh.cpp:886-896):
// 0,1 = bottom (-Z); 2,3 = top (+Z); 4,5 = +X; 6,7 = -Y; 8,9 = -X; 10,11 = +Y.
// (Facets 6,7 are {1,7,6} and {1,6,2}, whose three vertices all have y = 0; 10,11 are {4,0,3} and {4,3,5},
// all at y = y. Same table as test_paint_depth_clamp.cpp:39-52, which names the Y pair by its plane.)
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

// A box x*y*z whose four SIDE faces are each split by a ring of vertices at half height. A plain box's
// vertical edges run corner to corner, so every boundary vertex of a painted side face also touches the top
// or the bottom; this is the smallest fixture that offers a boundary vertex whose whole neighbourhood is
// vertical - Ruling 25's tie. All faces CCW outward.
static TriangleMesh make_ringed_box(double x, double y, double z)
{
    indexed_triangle_set its;
    auto V = [&](double px, double py, double pz) { its.vertices.emplace_back(float(px), float(py), float(pz)); return int(its.vertices.size()) - 1; };
    const double corner[4][2] = {{0., 0.}, {x, 0.}, {x, y}, {0., y}};    // CCW seen from +Z
    int lo[4], mid[4], hi[4];
    for (int i = 0; i < 4; ++i) lo[i]  = V(corner[i][0], corner[i][1], 0.);
    for (int i = 0; i < 4; ++i) mid[i] = V(corner[i][0], corner[i][1], z / 2.);
    for (int i = 0; i < 4; ++i) hi[i]  = V(corner[i][0], corner[i][1], z);
    auto T = [&](int a, int b, int c) { its.indices.emplace_back(a, b, c); };
    T(lo[0], lo[2], lo[1]); T(lo[0], lo[3], lo[2]);                      // bottom (-Z)
    T(hi[0], hi[1], hi[2]); T(hi[0], hi[2], hi[3]);                      // top (+Z)
    for (int i = 0; i < 4; ++i) {                                        // each side, two stacked quads
        const int j = (i + 1) % 4;
        T(lo[i],  lo[j],  mid[j]); T(lo[i],  mid[j], mid[i]);
        T(mid[i], mid[j], hi[j]);  T(mid[i], hi[j],  hi[i]);
    }
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
    // (+-1,+-1,1)/sqrt(3) bisectors. Spec 3.4a (Ruling 24): d = 1.5 is a depth PERPENDICULAR to the patch, so a
    // segment travelling along the bisector is mitred to d / (n(v).n_P) = 1.5 * sqrt(3) = 2.598mm long, which is
    // 1.5mm in x, in y AND in z - the 45 degree Voronoi diagonal the 2D segmentation produces. The slab is the
    // frustum between the 40x40 top and a 37x37 bottom 1.5mm below it. (Without the mitre the corners moved only
    // 1.5/sqrt(3) = 0.866mm per axis and the piece was a third short of its claim.)
    // Square frustum: h/3 * (A_top + A_bottom + sqrt(A_top*A_bottom)) = 0.5 * (1600 + 1369 + 1480).
    const double frustum = 1.5 / 3. * (1600. + 37. * 37. + 40. * 37.);      // = 2224.5 mm^3
    REQUIRE(c.volume < 40. * 40. * 1.5);                                    // strictly inside the straight prism
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

    // The same fixture with spec 3.6's crease step on: every boundary vertex now carries a ring copy too,
    // including BOTH wedges of the pinch, and the shell must still be valid. The painted region lies inside
    // one flat face, so its boundary is plain - the ring lands on the straight n(v) path and adds no volume.
    ColorSplitParams stepped;                                          // crease_step on
    stepped.flat_cap = false;                                          // ... but keep the depth at D = 1.5
    std::vector<ColorShell> ss = build_color_shells(p, depths_for_test(1.5), stepped, nullptr);
    REQUIRE(ss.size() == 1);
    ShellCheck sc = check_shell(ss[0].mesh);
    REQUIRE(sc.closed);
    REQUIRE(!sc.self_intersects);
    REQUIRE_THAT(sc.volume, WithinRel(7. * side * side * 1.5, 0.02));
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
    // (+-1,+-1,1)/sqrt(3) bisector normals carry a mitred 0.7*sqrt(3) = 1.2124mm segment (spec 3.4a) - 0.7mm
    // of vertical drop, the override read as the perpendicular depth it is.
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = no_cap_no_step();
    params.depth_override_mm = 0.7;
    auto shells = build_color_shells(p, depths_for_test(std::numeric_limits<double>::infinity()), params, nullptr);
    REQUIRE(shells.size() == 1);
    float min_z = shells[0].mesh.vertices.front().z(), max_z = min_z;
    for (const Vec3f &v : shells[0].mesh.vertices) { min_z = std::min(min_z, v.z()); max_z = std::max(max_z, v.z()); }
    REQUIRE_THAT(max_z, WithinAbs(20.f, 1e-4f));
    REQUIRE_THAT(min_z, WithinAbs(20.f - 0.7f, 1e-4f));
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
    for (const ColorShell &sh : shells) {
        REQUIRE(sh.state == 2);
        ShellCheck c = check_shell(sh.mesh);   // one CGAL run per shell, not two
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
    }
    ColorSplitResult rb = split_volume_by_paint(bossed.its, paint, depths_for_test(1.5, 0.2, 0.87), ColorSplitParams{}, nullptr);
    REQUIRE(rb.pieces.size() == 1);
    const double exposed = PI * 1.0 * 1.0 * 3.0;                        // 1 mm of the cylinder is buried in the block
    REQUIRE(volume_of(rb.pieces[0].second) >= 0.95 * exposed);
    float zmin = 1e9f; for (const Vec3f &v : rb.pieces[0].second.vertices) zmin = std::min(zmin, v.z());
    REQUIRE(zmin >= 10.f - 1e-3f);                                       // Ruling 14: nothing painted below the block top
    WARN("S1 boss piece " << volume_of(rb.pieces[0].second) << " mm^3 of " << exposed << " exposed ("
         << 100. * volume_of(rb.pieces[0].second) / exposed << " %)");
}

// The lowest z of a finished shell - the depth a pinned rule produces.
static float min_z(const indexed_triangle_set &mesh)
{
    float z = 1e9f;
    for (const Vec3f &v : mesh.vertices) z = std::min(z, v.z());
    return z;
}

// Every vertex of `mesh` is one of `expected` and every one of `expected` was built exactly once. Used where
// a fixture's shell is small enough to pin completely: a per-vertex check catches a wall that leans the wrong
// way at ONE corner, which the extremum check it replaces (min z, min x per shell) could not.
static void require_vertices_are(const indexed_triangle_set &mesh, const std::vector<Vec3f> &expected, float tol)
{
    REQUIRE(mesh.vertices.size() == expected.size());
    auto is_near = [tol](const Vec3f &a, const Vec3f &b) { return (a - b).norm() < tol; };
    for (const Vec3f &v : mesh.vertices)
        REQUIRE(std::count_if(expected.begin(), expected.end(), [&](const Vec3f &e) { return is_near(v, e); }) == 1);
    for (const Vec3f &e : expected)
        REQUIRE(std::count_if(mesh.vertices.begin(), mesh.vertices.end(), [&](const Vec3f &v) { return is_near(v, e); }) == 1);
}

TEST_CASE("colorsplit: a painted cube top and side are two smooth patches with straight walls", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    std::vector<std::pair<int, EnforcerBlockerType>> facets = all_with(CUBE_TOP, EnforcerBlockerType::Extruder2);
    for (int f : CUBE_PLUS_X) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, facets));
    ColorSplitParams params = no_cap_no_step();
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(shells.size() == 2);
    // Every boundary vertex, not just the deepest one (Task 5 review follow-up). Each patch is one quad, so
    // its shell is exactly four top copies and four bottom copies. Ruling 19: a corner that carries the
    // SAME-STATE crease edge (the shared top/+X edge) walks straight down the patch's own normal - 1.5 in z
    // for the top slab, 1.5 in x for the side slab. A corner that carries none tapers along the cube's corner
    // bisector, and spec 3.4a (Ruling 24) mitres that segment to 1.5*sqrt(3) so its PERPENDICULAR depth is
    // 1.5 as well: 1.5 in each axis, the 45 degree diagonal. Every bottom copy of the top slab is therefore
    // at z = 18.5 and every bottom copy of the side slab at x = 38.5, by two different rules.
    auto has = [](const indexed_triangle_set &m, const Vec3f &q) {
        return std::any_of(m.vertices.begin(), m.vertices.end(), [&](const Vec3f &v) { return (v - q).norm() < 1e-3f; });
    };
    const bool zero_is_top = has(shells[0].mesh, Vec3f(0.f, 0.f, 20.f));
    const indexed_triangle_set &top_slab  = zero_is_top ? shells[0].mesh : shells[1].mesh;
    const indexed_triangle_set &side_slab = zero_is_top ? shells[1].mesh : shells[0].mesh;
    require_vertices_are(top_slab,
                         {{0.f, 0.f, 20.f}, {40.f, 0.f, 20.f}, {40.f, 40.f, 20.f}, {0.f, 40.f, 20.f},
                          {1.5f, 1.5f, 18.5f}, {40.f, 0.f, 18.5f}, {40.f, 40.f, 18.5f}, {1.5f, 38.5f, 18.5f}}, 1e-3f);
    require_vertices_are(side_slab,
                         {{40.f, 0.f, 0.f}, {40.f, 40.f, 0.f}, {40.f, 40.f, 20.f}, {40.f, 0.f, 20.f},
                          {38.5f, 0.f, 20.f}, {38.5f, 40.f, 20.f}, {38.5f, 1.5f, 1.5f}, {38.5f, 38.5f, 1.5f}}, 1e-3f);
    ColorSplitResult r = split_volume_by_paint(block.its, paint_data(block, facets), depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(r.pieces.size() == 1);
    double total = volume_of(r.body) + volume_of(r.pieces[0].second);
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
}

TEST_CASE("colorsplit: a plain painted face is D deep perpendicular to itself (mitred bisector)", "[colorsplit]")
{
    // Spec 3.4a (Ruling 24), pinned directly on the fixture that motivated it: a plain cube face whose only
    // vertices are its four corners. Their bisectors make an angle of acos(1/sqrt(3)) = 54.7 degrees with the
    // face, so an unmitred segment of length d buys only d/sqrt(3) = 0.866 of perpendicular depth. The mitre
    // divides by n(v).n_P, spending d * sqrt(3) = 2.598 along the bisector instead, which lands every bottom
    // copy exactly d = 1.5 behind the face and exactly d in from the two faces the corner shares - the 45
    // degree Voronoi diagonal the 2D segmentation draws at the same edge. Nothing is clamped here: the
    // half-thickness along the corner diagonal of a 40x40x20 block is 17.3 mm.
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    require_vertices_are(shells[0].mesh,
                         {{40.f, 0.f, 0.f}, {40.f, 40.f, 0.f}, {40.f, 40.f, 20.f}, {40.f, 0.f, 20.f},
                          {38.5f, 1.5f, 1.5f}, {38.5f, 38.5f, 1.5f}, {38.5f, 38.5f, 18.5f}, {38.5f, 1.5f, 18.5f}}, 1e-4f);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    // ... and the solid between the 40x20 face and the 37x17 rectangle 1.5mm behind it is that prismatoid:
    // h/6 * (A_top + 4*A_mid + A_bottom) with A_mid = 38.5 x 18.5, i.e. 1069.5 mm^3. (The pyramid-frustum
    // formula does not apply - 40:20 and 37:17 are not similar rectangles.)
    REQUIRE_THAT(c.volume, WithinRel(1.5 / 6. * (40. * 20. + 4. * 38.5 * 18.5 + 37. * 17.), 1e-4));
}

// ---- Spec 3.5 (flat cap) and 3.6 (crease step), both on by default. ----

static ColorSplitParams cap_and_step() { return ColorSplitParams{}; }

TEST_CASE("colorsplit: flat top is capped at the solid shell depth, slopes are not", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.1, 0.87);   // cap_top = 0.8 set in depths_for_test
    d.cap_top = 0.6;
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorPatches p = extract_color_patches(block.its, data);

    // Crease step OFF isolates the cap. The only vertices of a plain cube's top face are its four corners,
    // whose angle-weighted normal is the (+-1,+-1,1)/sqrt(3) bisector; spec 3.4a mitres the segment along it
    // to 0.6 * sqrt(3), which is 0.6 mm of z. The cap depth is what this pins (uncapped would be 1.5) - and
    // since the mitre already makes the perpendicular depth exact, the step does not change the number.
    ColorSplitParams params = cap_and_step(); params.crease_step = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    REQUIRE(shells[0].capped);
    REQUIRE_THAT(min_z(shells[0].mesh), WithinAbs(20.f - 0.6f, 1e-4f));

    // Crease step ON (the shipping default): spec 3.6 case A walks the rim straight down n_P after one layer,
    // so the same cap depth shows up as a vertical 0.6 - the 19.4 the brief pins, by a different route.
    auto stepped = build_color_shells(p, d, cap_and_step(), nullptr);
    REQUIRE(stepped.size() == 1);
    REQUIRE(stepped[0].capped);
    REQUIRE_THAT(min_z(stepped[0].mesh), WithinAbs(20.f - 0.6f, 1e-4f));

    // Spec 3.5's second gate: a cap that is not shallower than D has nothing to cap.
    ColorSplitDepths deep = d; deep.cap_top = 1.5;
    auto uncapped = build_color_shells(p, deep, cap_and_step(), nullptr);
    REQUIRE(uncapped.size() == 1);
    REQUIRE(!uncapped[0].capped);
    REQUIRE_THAT(min_z(uncapped[0].mesh), WithinAbs(20.f - 1.5f, 1e-4f));

    // 3 degree slope: a 40x40 wedge whose top rises 40*tan(3deg): NOT flat (tan 3deg = 0.052 > h/(3ws) = 0.038)
    indexed_triangle_set wedge = its_make_cube(40., 40., 20.);
    for (Vec3f &v : wedge.vertices) if (v.z() > 19.f) v.z() += float(v.x() * std::tan(3. * PI / 180.));
    TriangleMesh wedge_mesh(wedge);
    auto wd = paint_by_predicate(wedge_mesh, [](const Vec3f &c, const Vec3f &n) { return n.z() > 0.9f; }, EnforcerBlockerType::Extruder2);
    ColorPatches pw = extract_color_patches(wedge_mesh.its, wd);
    auto ws_ = build_color_shells(pw, d, params, nullptr);
    REQUIRE(ws_.size() == 1);
    REQUIRE(!ws_[0].capped);

    // 1 degree slope IS flat (tan 1deg = 0.017 < 0.038)
    indexed_triangle_set wedge1 = its_make_cube(40., 40., 20.);
    for (Vec3f &v : wedge1.vertices) if (v.z() > 19.f) v.z() += float(v.x() * std::tan(1. * PI / 180.));
    TriangleMesh w1(wedge1);
    ColorPatches p1 = extract_color_patches(w1.its, paint_by_predicate(w1, [](const Vec3f &, const Vec3f &n) { return n.z() > 0.9f; }, EnforcerBlockerType::Extruder2));
    REQUIRE(build_color_shells(p1, d, params, nullptr)[0].capped);
}

TEST_CASE("colorsplit: a painted flat bottom is capped at the bottom shell depth", "[colorsplit]")
{
    // The down-facing half of spec 3.5: the same rule with n_z < 0, cap_bottom instead of cap_top (0.6 in
    // depths_for_test), and an XY projection that comes out clockwise until the core gate flips its winding.
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_BOTTOM, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, depths_for_test(1.5), cap_and_step(), nullptr);
    REQUIRE(shells.size() == 1);
    REQUIRE(shells[0].capped);
    float zmax = -1e9f;
    for (const Vec3f &v : shells[0].mesh.vertices) zmax = std::max(zmax, v.z());
    REQUIRE_THAT(zmax, WithinAbs(0.6f, 1e-4f));      // case A walks straight UP n_P here, by cap_bottom
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
}

TEST_CASE("colorsplit: narrow flat strip (core < 3 wall stacks) is not capped", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 2., 20., 1, 1);   // top is 40 x 2mm: inward offset by 1.5*0.87 = 1.3 kills it
    auto data = paint_data(box, {{2, EnforcerBlockerType::Extruder2}, {3, EnforcerBlockerType::Extruder2}});
    ColorPatches p = extract_color_patches(box.its, data);
    ColorSplitParams params = cap_and_step(); params.crease_step = false;
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.1, 0.87), params, nullptr);
    REQUIRE(shells.size() == 1);
    REQUIRE(!shells[0].capped);
}

TEST_CASE("colorsplit: painted top steps one wall stack in below the surface layer at side faces", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = cap_and_step(); params.flat_cap = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    // Ring vertices at z = 20 - 0.2 must be inset 0.87 from the side faces; bottom vertices at z = 20 - 1.5 too.
    // (Case A mitres the inward tangent at a corner, so the inset holds against BOTH side faces meeting there
    // rather than measuring 0.87 along the diagonal.)
    int rings = 0, bottoms = 0;
    for (const Vec3f &v : shells[0].mesh.vertices) {
        if (std::abs(v.z() - 19.8f) < 1e-4f || std::abs(v.z() - 18.5f) < 1e-4f) {
            REQUIRE((std::abs(v.x() - 0.87f) < 1e-3f || std::abs(v.x() - (40.f - 0.87f)) < 1e-3f));
            REQUIRE((std::abs(v.y() - 0.87f) < 1e-3f || std::abs(v.y() - (40.f - 0.87f)) < 1e-3f));
            (std::abs(v.z() - 19.8f) < 1e-4f ? rings : bottoms)++;
        }
    }
    REQUIRE(rings == 4);      // one ring copy and one bottom copy per corner of the painted top
    REQUIRE(bottoms == 4);
    REQUIRE(check_shell(shells[0].mesh).closed);
}

TEST_CASE("colorsplit: the crease step's own descent stops at mid-thickness on a thin plate", "[colorsplit]")
{
    // Ruling 20: case A descends along n_P, but d(v) was measured along n(v). At this plate's corners the
    // bisector probe gives d = 1.2*sqrt(3)/2 - delta = 1.037, while the material under the wall - straight
    // down from the ring, one wall stack inside the rim - is only 1.2 mm thick, so the wall may descend
    // 0.598. Without the re-probe the bottom would land at z = 1.0 - (1.037 - 0.2) = 0.163, well past the
    // mid-surface and into the shell the other side would build.
    ColorSplitDepths d     = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh     plate = make_cube(40., 40., 1.2);
    ColorPatches     p     = extract_color_patches(plate.its, paint_data(plate, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    for (bool flat_cap : {false, true}) {                  // the cap (0.8) is the looser of the two clamps here
        ColorSplitParams params = cap_and_step();
        params.flat_cap = flat_cap;
        auto shells = build_color_shells(p, d, params, nullptr);
        REQUIRE(shells.size() == 1);
        int tops = 0, rings = 0, bottoms = 0;
        for (const Vec3f &v : shells[0].mesh.vertices) {
            if      (std::abs(v.z() - 1.2f) < 1e-3f) ++tops;
            else if (std::abs(v.z() - 1.0f) < 1e-3f) ++rings;
            else { REQUIRE_THAT(v.z(), WithinAbs(0.602f, 1e-3f)); ++bottoms; }
        }
        REQUIRE(tops == 4);
        REQUIRE(rings == 4);
        REQUIRE(bottoms == 4);
        ShellCheck c = check_shell(shells[0].mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
    }
}

TEST_CASE("colorsplit: the wall-stack step never crosses a thin wall's mid-plane", "[colorsplit]")
{
    // Ruling 21: case B steps a wall stack straight in along n_P, but d(v) is measured along n(v). This
    // wall's top-edge corners get d = 1*sqrt(3)/2 - delta = 0.864 from their bisector probe while the wall is
    // 1 mm thick, so an unclamped step (min(d, ws) = 0.864) would land at y = 0.136 - and the taper after it
    // further still. The step and the taper share one budget now: the n_P mid-thickness, 0.498.
    const std::vector<int> PLUS_Y{10, 11}, MINUS_Y{6, 7};
    ColorSplitDepths d    = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh     wall = make_cube(40., 1.0, 20.);
    ColorPatches     p    = extract_color_patches(wall.its, paint_data(wall, all_with(PLUS_Y, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, d, cap_and_step(), nullptr);
    REQUIRE(shells.size() == 1);
    for (const Vec3f &v : shells[0].mesh.vertices)
        REQUIRE(v.y() >= 0.5f - 0.002f - 1e-4f);     // never past the mid-plane, less spec 3.4's delta
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);

    // Both faces painted, one filament each: the two slabs stop 2*delta short of each other (spec 3.4), so
    // neither loses its band and body + pieces still add up to the whole wall.
    std::vector<std::pair<int, EnforcerBlockerType>> both = all_with(PLUS_Y, EnforcerBlockerType::Extruder2);
    for (int f : MINUS_Y) both.emplace_back(f, EnforcerBlockerType::Extruder3);
    ColorPatches p2 = extract_color_patches(wall.its, paint_data(wall, both));
    REQUIRE(build_color_shells(p2, d, cap_and_step(), nullptr).size() == 2);
    ColorSplitResult r = split_volume_by_paint(wall.its, paint_data(wall, both), d, cap_and_step(), nullptr);
    REQUIRE(r.pieces.size() == 2);
    const double wall_volume = 40. * 1.0 * 20.;
    double       total       = volume_of(r.body);
    for (const std::pair<int, indexed_triangle_set> &piece : r.pieces) {
        REQUIRE(volume_of(piece.second) >= 0.45 * wall_volume);      // neither filament loses its band
        total += volume_of(piece.second);
    }
    REQUIRE_THAT(total, WithinRel(wall_volume, 1e-4));
}

TEST_CASE("colorsplit: a same-state wall on a thin wall stops at the mid-plane too", "[colorsplit]")
{
    // The always-on half of spec 3.6 has the same mid-thickness clamp as the crease step, and the same probe:
    // launched from the boundary vertex it grazed the faces that vertex also sits on, found nothing, and left
    // the clamp at +inf - so a same-state wall on a 1 mm wall walked to y = 1 - 0.864 = 0.136. Painting the
    // +Y face AND the top with one filament puts a same-state crease along their shared top edge, which is
    // exactly the boundary that reaches this rule (the side patch's own bottom corners are case B).
    const std::vector<int> PLUS_Y{10, 11};
    ColorSplitDepths d    = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh     wall = make_cube(40., 1.0, 20.);
    std::vector<std::pair<int, EnforcerBlockerType>> facets = all_with(PLUS_Y, EnforcerBlockerType::Extruder2);
    for (int f : CUBE_TOP) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(wall.its, paint_data(wall, facets));
    auto shells = build_color_shells(p, d, cap_and_step(), nullptr);
    REQUIRE(shells.size() == 2);                       // the top and the side are two smooth patches
    auto has = [](const indexed_triangle_set &m, const Vec3f &q) {
        return std::any_of(m.vertices.begin(), m.vertices.end(), [&](const Vec3f &v) { return (v - q).norm() < 1e-3f; });
    };
    const indexed_triangle_set &side = has(shells[0].mesh, Vec3f(0.f, 1.f, 0.f)) ? shells[0].mesh : shells[1].mesh;
    REQUIRE(has(side, Vec3f(0.f, 1.f, 0.f)));          // the side patch reaches the build plate, the top does not
    for (const Vec3f &v : side.vertices)
        REQUIRE(v.y() >= 0.5f - 0.002f - 1e-4f);
    ShellCheck c = check_shell(side);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
}

TEST_CASE("colorsplit: a painted top narrower than two wall stacks falls back to the bisector", "[colorsplit]")
{
    // Ruling 22: case A insets the ring one wall stack from every side, which on a group narrower than 2 ws
    // makes the two insets cross - the ring inverts, the fold guard fights it and the group is dropped. The
    // group's own outline decides: when it cannot survive an inward offset of ws, its case A vertices use the
    // plain bisector rule instead. This top is 1.5 mm wide, so 2 * 0.87 does not fit.
    ColorSplitDepths d     = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh     block = make_cube(40., 1.5, 20.);
    ColorPatches     p     = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<std::string> warnings;
    auto shells = build_color_shells(p, d, cap_and_step(), nullptr, &warnings);
    REQUIRE(shells.size() == 1);
    REQUIRE(warnings.empty());                        // no "too small to split" - the shell is built, not lost
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    REQUIRE(c.volume > 0.);
    // Plain bisector rule at all four corners: the ring one layer down it, the bottom the full depth down it.
    // (The flat cap does not fire either - 1.5 mm is under its 3-wall-stack core gate.)
    REQUIRE(!shells[0].capped);
    const float dep = float(1.5 * std::sqrt(3.) / 2. - 0.002);   // half the bisector thickness of a 1.5 mm wall
    // Spec 3.4a mitres both segments, then clamps each by the half-thickness along the bisector. The ring's
    // one layer becomes 0.2*sqrt(3) long - a vertical 0.2, one real layer down - while the bottom's 1.29704
    // is ALREADY the clamp (1.29704 * sqrt(3) = 2.247 is far past it), so the bottom does not move at all:
    // this fixture's corners are held by their own mid-thickness, not by D.
    const float bot = dep / std::sqrt(3.f), rng = 0.2f;          // spread over x, y and z alike
    require_vertices_are(shells[0].mesh,
                         {{0.f, 0.f, 20.f}, {40.f, 0.f, 20.f}, {40.f, 1.5f, 20.f}, {0.f, 1.5f, 20.f},
                          {rng, rng, 20.f - rng}, {40.f - rng, rng, 20.f - rng},
                          {40.f - rng, 1.5f - rng, 20.f - rng}, {rng, 1.5f - rng, 20.f - rng},
                          {bot, bot, 20.f - bot}, {40.f - bot, bot, 20.f - bot},
                          {40.f - bot, 1.5f - bot, 20.f - bot}, {bot, 1.5f - bot, 20.f - bot}}, 1e-3f);
}

TEST_CASE("colorsplit: painted side face keeps its full wall stack up to the top edge", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = cap_and_step(); params.flat_cap = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    // Ring vertices at the top edge: x = 40 - 0.87, z = 20 (no downward move); bottom vertices then taper along the bisector.
    bool found_ring = false;
    for (const Vec3f &v : shells[0].mesh.vertices)
        if (std::abs(v.x() - (40.f - 0.87f)) < 1e-3f && std::abs(v.z() - 20.f) < 1e-4f) found_ring = true;
    REQUIRE(found_ring);
    REQUIRE(check_shell(shells[0].mesh).closed);

    // d <= ws: the wall stack alone uses up the whole depth, so the second strip has no room, the bottom
    // collapses onto the ring and the side is a slab of depth d straight in along n_P with no taper at all.
    auto shallow = build_color_shells(p, depths_for_test(0.5, 0.2, 0.87), params, nullptr);
    REQUIRE(shallow.size() == 1);
    for (const Vec3f &v : shallow[0].mesh.vertices)
        REQUIRE((std::abs(v.x() - 40.f) < 1e-4f || std::abs(v.x() - 39.5f) < 1e-4f));
    REQUIRE(check_shell(shallow[0].mesh).closed);
}

TEST_CASE("colorsplit: a vertical crease between two side faces takes the plain bisector, not case B", "[colorsplit]")
{
    // Ruling 25. Spec 3.6's two convex cases are about the ASYMMETRY between a painted face and a neighbour
    // that is more (case A) or less (case B) horizontal than it: case B exists so a painted SIDE face keeps
    // its full outer wall stack up to the TOP edge. Between two vertical faces there is no such asymmetry -
    // and the 2D segmentation just draws its 45 degree Voronoi diagonal down the shared edge - so a tie in
    // |n_P.z| vs |n_Q.z| must not fall into case B by the tie-break of a strict comparison.
    //
    // This box's side faces are split at half height, so the +X patch has SIX boundary vertices of two kinds:
    //  * the four corners, which each carry one vertical boundary edge AND one horizontal one (against the
    //    top or bottom face), so |n_Q.z| = 0.707 against the patch's 0 - genuine case B, ring a wall stack
    //    in along n_P and then the mitred taper: (39.13, 0, 0) -> (38.5, 0.63, 0.63).
    //  * the two at half height, whose only neighbour is the vertical face across the edge, |n_Q.z| = 0 = the
    //    patch's own - the TIE, which now takes the plain mitred bisector: n(v) = (1,-1,0)/sqrt(2), the ring
    //    0.2*sqrt(2) down it (0.2 per axis) and the bottom 1.5*sqrt(2) (1.5 per axis), landing on the 45
    //    degree diagonal at (38.5, 1.5, 10). Before Ruling 25 it was case B and landed at (38.5, 0.63, 10).
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh     box = make_ringed_box(40., 40., 20.);
    ColorPatches     p   = extract_color_patches(box.its, paint_by_predicate(box, [](const Vec3f &, const Vec3f &n) { return n.x() > 0.9f; }, EnforcerBlockerType::Extruder2));
    auto shells = build_color_shells(p, d, cap_and_step(), nullptr);
    REQUIRE(shells.size() == 1);                       // the two stacked quads are one smooth patch
    require_vertices_are(shells[0].mesh,
                         {{40.f, 0.f, 0.f},  {40.f, 40.f, 0.f},  {40.f, 0.f, 10.f},
                          {40.f, 40.f, 10.f}, {40.f, 0.f, 20.f}, {40.f, 40.f, 20.f},
                          {39.13f, 0.f, 0.f},  {39.13f, 40.f, 0.f},                       // case B rings
                          {39.13f, 0.f, 20.f}, {39.13f, 40.f, 20.f},
                          {39.8f, 0.2f, 10.f}, {39.8f, 39.8f, 10.f},                      // tie: plain ring
                          {38.5f, 0.63f, 0.63f},   {38.5f, 39.37f, 0.63f},                // case B bottoms
                          {38.5f, 0.63f, 19.37f},  {38.5f, 39.37f, 19.37f},
                          {38.5f, 1.5f, 10.f}, {38.5f, 38.5f, 10.f}}, 1e-3f);             // tie: 45 deg diagonal
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
}

TEST_CASE("colorsplit: a capped group and the uncapped group beside it meet along a straight wall", "[colorsplit]")
{
    // Both options on, the shipping default, on the fixture that carries all four spec 3.6 cases at once:
    // the cube's top (a capped flat group) and its +X face (uncapped) are painted the same filament.
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);          // cap_top = 0.8
    TriangleMesh block = make_cube(40., 40., 20.);
    std::vector<std::pair<int, EnforcerBlockerType>> facets = all_with(CUBE_TOP, EnforcerBlockerType::Extruder2);
    for (int f : CUBE_PLUS_X) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, facets));
    auto shells = build_color_shells(p, d, cap_and_step(), nullptr);
    REQUIRE(shells.size() == 2);
    REQUIRE(shells[0].capped);        // spec 3.8: within a filament the capped groups come first
    REQUIRE(!shells[1].capped);
    // Case B: how far the mitred bisector taper below the ring reaches. Its length is (1.5 - 0.87)*sqrt(3),
    // so it spends 1.5 - 0.87 = 0.63 on each axis: the piece's depth along n_P is the wall stack plus that,
    // i.e. exactly D, and the taper is the 45 degree diagonal (spec 3.4a).
    const float rim = 1.5f - 0.87f;
    // Top slab, capped at 0.8: the two corners on the shared (same-state) edge walk straight down 0.8, the
    // two at x = 0 take case A - a mitred 0.87 inward and one layer down, then straight down n_P by the rest.
    require_vertices_are(shells[0].mesh,
                         {{0.f, 0.f, 20.f}, {40.f, 0.f, 20.f}, {40.f, 40.f, 20.f}, {0.f, 40.f, 20.f},
                          {0.87f, 0.87f, 19.8f}, {0.87f, 39.13f, 19.8f},
                          {0.87f, 0.87f, 19.2f}, {0.87f, 39.13f, 19.2f}, {40.f, 0.f, 19.2f}, {40.f, 40.f, 19.2f}}, 1e-3f);
    // Side slab, uncapped: the two corners on the shared edge walk straight in 1.5 (the top slab claims the
    // material right behind that wall), the two at z = 0 take case B - a full wall stack in along n_P, then
    // the taper along the corner bisector.
    require_vertices_are(shells[1].mesh,
                         {{40.f, 0.f, 0.f}, {40.f, 40.f, 0.f}, {40.f, 40.f, 20.f}, {40.f, 0.f, 20.f},
                          {39.13f, 0.f, 0.f}, {39.13f, 40.f, 0.f},
                          {38.5f, 0.f, 20.f}, {38.5f, 40.f, 20.f}, {39.13f - rim, rim, rim}, {39.13f - rim, 40.f - rim, rim}}, 1e-3f);
    ColorSplitResult r = split_volume_by_paint(block.its, paint_data(block, facets), d, cap_and_step(), nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE_THAT(volume_of(r.body) + volume_of(r.pieces[0].second), WithinRel(40. * 40. * 20., 1e-4));
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
    // Two, not one, since spec 3.5's flat cap went in: the painted spherical cap is a single smooth patch,
    // but its CROWN is flat to within h/(3ws) = 4.38 degrees, i.e. a disc of radius 20*sin(4.38) = 1.53 mm,
    // which survives the 1.305 mm core offset and becomes a capped group at cap_top. That is the dome-crown
    // split the 2D rule makes too (MultiMaterialSegmentation.cpp:2228's comment: the crown is capped, the rim
    // rolling into the flank is not); the flank is the second, uncapped group.
    REQUIRE(shells.size() == 2);
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
    size_t shell_tri = 0; for (const ColorShell &s : shells) shell_tri += s.mesh.indices.size();
    WARN("S3 breakdown (" << sphere.its.indices.size() << " tri, shells " << shell_tri << " tri): patches "
         << patches_s << " s, shells " << shells_s << " s (of which check_shell/CGAL " << check_s << " s = "
         << (shells_s > 0. ? 100. * check_s / shells_s : 0.) << "% of that stage, "
         << (100. * check_s / std::max(1e-9, patches_s + shells_s + partition_s)) << "% of the whole split), partition "
         << partition_s << " s; shell warnings " << warnings.size());
    for (const std::string &w : warnings) WARN("S3 breakdown warning: " << w);
}

// ---------------------------------------------------------------------------------------------------------
// Task 7 - spec 3.9 (coordinate space) and spec 4 (model changes).

// An object carrying ONE painted MODEL_PART: the shape apply_color_split is asked to replace.
static Model painted_model(const TriangleMesh &mesh, const std::vector<std::pair<int, EnforcerBlockerType>> &facets, int extruder = 1)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "split-test";
    ModelVolume *volume = object->add_volume(mesh);
    // add_volume(const TriangleMesh &) leaves the volume unnamed (Model.hpp:1090), so name it here - the
    // body's name is asserted against the SOURCE VOLUME's name, not the object's.
    volume->name = "split-test";
    volume->config.set("extruder", extruder);
    // add_volume centres the mesh, which is a translation: facet indices (and so CUBE_TOP) still hold.
    TriangleSelector selector(volume->mesh());
    for (auto [f, st] : facets) selector.set_facet(f, st);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();
    return model;
}

static BoundingBoxf3 world_bbox(const ModelObject &o) { return o.instance_bounding_box(0, false); }

TEST_CASE("colorsplit: apply replaces the source by body + parts in place with extruders set", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    const ObjectID src_id = src.id();
    BoundingBoxf3 before = world_bbox(object);
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 0, std::move(r), space, /*solid_interfaces=*/true, /*keep_base_sparse=*/false);
    REQUIRE(object.volumes.size() == 2);
    REQUIRE(created.size() == 2);
    REQUIRE(object.volumes[0] == created[0]);
    REQUIRE(object.volumes[1] == created[1]);
    REQUIRE(object.volumes[0]->name == "split-test");
    REQUIRE(object.volumes[1]->name == "split-test F2");
    REQUIRE(object.volumes[0]->config.has("extruder"));
    REQUIRE(object.volumes[0]->config.extruder() == 1);
    REQUIRE(object.volumes[1]->config.extruder() == 2);
    REQUIRE(object.volumes[0]->is_model_part());
    REQUIRE(object.volumes[1]->is_model_part());
    REQUIRE(!object.volumes[0]->is_mm_painted());
    REQUIRE(!object.volumes[1]->is_mm_painted());
    REQUIRE(object.config.get().opt_bool("interface_shells"));
    // Undo/redo safety: every new volume (and its config) carries its own fresh ObjectID.
    REQUIRE(created[0]->id() != src_id);
    REQUIRE(created[1]->id() != src_id);
    REQUIRE(created[0]->id() != created[1]->id());
    REQUIRE(created[0]->config.id() != created[1]->config.id());
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-4);
    REQUIRE((after.max - before.max).norm() < 1e-4);
}

TEST_CASE("colorsplit: a rotated, scaled and mirrored PART stays in place", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    src.set_rotation(Vec3d(0.3, 0.2, 0.7));
    src.set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    src.set_mirror(Vec3d(-1., 1., 1.));
    src.set_offset(Vec3d(5., -3., 2.));
    object.invalidate_bounding_box();
    BoundingBoxf3 before = world_bbox(object);
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(!space.world_path);                       // isotropic: mesh-space path
    REQUIRE_THAT(space.depth_scale, WithinRel(1.5, 1e-5));
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
}

// The whole top face of a grid box: its INTERIOR vertices carry vertical normals, so they walk straight down
// and the piece is a flat-topped slab exactly D deep in z, whatever the mitre does. A plain make_cube's top
// has nothing but corner vertices: since Ruling 24 their bisectors are mitred, so they too bury D
// perpendicular to the top face - but each also travels D inward in x and y, which makes the piece a tapered
// wedge rather than the full-footprint slab these bounding-box measurements want.
static std::vector<std::pair<int, EnforcerBlockerType>> grid_box_top(int n)
{
    std::vector<int> top;
    for (int f = 2; f < 2 + 2 * n * n; ++f) top.push_back(f);
    return all_with(top, EnforcerBlockerType::Extruder2);
}

TEST_CASE("colorsplit: anisotropic instance scale takes the world path", "[colorsplit]")
{
    Model model = painted_model(make_grid_box(40., 40., 20., 6, 6), grid_box_top(6));
    ModelObject &object = *model.objects.front();
    object.instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    ModelVolume &src = *object.volumes.front();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(space.world_path);
    // Ruling 23: mesh and paint go in as they stand; the split carries the retriangulated surface across.
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               depths_for_test(1.5), no_cap_no_step(), nullptr, space.to_split);
    BoundingBoxf3 before = world_bbox(object);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
    // The piece is 1.5mm deep in WORLD z (instance scale is in x only) and, back in mesh space, spans the
    // full 40mm top - proof that from_split undid the x2 the split ran under.
    const ModelVolume *piece = object.volumes.back();
    BoundingBoxf3 pb = piece->mesh().bounding_box();
    REQUIRE_THAT(pb.size().z(), WithinRel(1.5, 0.02));
    REQUIRE_THAT(pb.size().x(), WithinRel(40., 0.02));
}

TEST_CASE("colorsplit: the world path brings a mirrored part back outward-facing", "[colorsplit]")
{
    // Anisotropic instance AND a mirrored volume: det(T) < 0, so both legs of the round trip flip the
    // winding. Getting only one of them right would hand the model an inside-out (negative volume) part.
    Model model = painted_model(make_grid_box(40., 40., 20., 6, 6), grid_box_top(6));
    ModelObject &object = *model.objects.front();
    object.instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    ModelVolume &src = *object.volumes.front();
    src.set_mirror(Vec3d(-1., 1., 1.));
    object.invalidate_bounding_box();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(space.world_path);
    REQUIRE(space.to_split.linear().determinant() < 0.);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               depths_for_test(1.5), no_cap_no_step(), nullptr, space.to_split);
    BoundingBoxf3 before = world_bbox(object);
    auto created = apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
    for (const ModelVolume *v : created)
        REQUIRE(its_volume(v->mesh().its) > 0.f);     // outward winding survived the round trip
}

TEST_CASE("colorsplit: empty body removes the source and keeps only pieces", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(sphere);
    TriangleSelector selector(volume->mesh());
    for (int f = 0; f < int(volume->mesh().its.indices.size()); ++f) selector.set_facet(f, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    ColorSplitSpace space = color_split_space(*object, *volume);
    ColorSplitResult r = split_volume_by_paint(volume->mesh().its, volume->mmu_segmentation_facets.get_data(),
                                               depths_for_test(1.5), ColorSplitParams{}, nullptr);
    REQUIRE(r.body.indices.empty());
    apply_color_split(*object, 0, std::move(r), space, false, false);
    REQUIRE(object->volumes.size() == 1);
    REQUIRE(object->volumes[0]->config.extruder() == 2);
}

TEST_CASE("colorsplit: the new volumes take the source's slot, between the modifiers around it", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume *first_mod = object.add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);
    first_mod->name = "stay-first";
    ModelVolume *last_mod = object.add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);
    last_mod->name = "stay-last";
    std::swap(object.volumes[0], object.volumes[1]);   // [modifier, source, modifier]: src_idx is NOT zero
    ModelVolume &src = *object.volumes[1];
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 1, std::move(r), space, false, false);
    REQUIRE(created.size() == 2);
    REQUIRE(object.volumes.size() == 4);
    REQUIRE(object.volumes[0] == first_mod);           // the volumes around the source keep their order
    REQUIRE(object.volumes[1] == created[0]);          // body first, so slice-time clipping order holds
    REQUIRE(object.volumes[2] == created[1]);
    REQUIRE(object.volumes[3] == last_mod);
    REQUIRE(object.volumes[0]->is_modifier());
    REQUIRE(object.volumes[3]->is_modifier());
    REQUIRE(!object.config.get().has("interface_shells"));   // solid_interfaces = false touches nothing
}

TEST_CASE("colorsplit: a source with no extruder key of its own leaves the body inheriting the object's", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    object.config.set("extruder", 3);
    ModelVolume &src = *object.volumes.front();
    src.config.erase("extruder");
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 0, std::move(r), space, false, /*keep_base_sparse_infill=*/true);
    REQUIRE(created.size() == 2);
    // No explicit key on the body: it inherits the object's, so the Ultra outer_wall_filament reset
    // (PrintObject.cpp:3257-3259) is not re-triggered.
    REQUIRE(!created[0]->config.has("extruder"));
    REQUIRE(created[0]->extruder_id() == 3);
    REQUIRE(created[1]->config.extruder() == 2);
    // "Keep base-colour sparse infill": the part's interior stays the body's filament (PrintApply.cpp:1096-1103).
    REQUIRE(created[1]->config.opt_int("sparse_infill_filament") == 3);
    REQUIRE(!created[0]->config.has("sparse_infill_filament"));
}

TEST_CASE("colorsplit: an empty result leaves the object untouched", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume *src = object.volumes.front();
    ColorSplitSpace space = color_split_space(object, *src);
    auto created = apply_color_split(object, 0, ColorSplitResult{}, space, true, false);
    REQUIRE(created.empty());
    REQUIRE(object.volumes.size() == 1);
    REQUIRE(object.volumes[0] == src);                       // nothing produced, nothing deleted
    REQUIRE(!object.config.get().has("interface_shells"));
}

TEST_CASE("colorsplit: the depth override is a WORLD length on the mesh-space path", "[colorsplit]")
{
    // effective_depths applies params.depth_override_mm INSIDE the split, after the caller has scaled the
    // depths, so on the mesh-space path it has to be scaled by the caller too - otherwise a 2x volume cuts
    // 1 mm of MESH, i.e. 2 mm of world. scale_params is that scaling.
    Model model = painted_model(make_grid_box(40., 40., 20., 6, 6), grid_box_top(6));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    src.set_scaling_factor(Vec3d(2., 2., 2.));
    object.invalidate_bounding_box();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(!space.world_path);
    REQUIRE_THAT(space.depth_scale, WithinRel(2., 1e-9));
    ColorSplitParams params = no_cap_no_step();
    params.depth_override_mm = 1.0;                                   // world millimetres, as the dialog gives it
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale),
                                               scale_params(params, space.depth_scale), nullptr);
    // The result reports the depths it CUT with, i.e. in split space: half a millimetre of mesh.
    REQUIRE_THAT(r.depths.D, WithinRel(0.5, 1e-9));
    REQUIRE_THAT(r.depths.D * space.depth_scale, WithinRel(1.0, 1e-9));
    auto created = apply_color_split(object, 0, std::move(r), space, false, false);
    REQUIRE(created.size() == 2);
    const ModelVolume *piece = created.back();
    REQUIRE_THAT(piece->mesh().bounding_box().size().z(), WithinRel(0.5, 0.02));        // mesh millimetres
    const BoundingBoxf3 wb = piece->mesh().transformed_bounding_box(object.instances.front()->get_matrix() * piece->get_matrix());
    REQUIRE_THAT(wb.size().z(), WithinRel(1.0, 0.02));                                  // world millimetres
}

TEST_CASE("colorsplit: a partially painted facet is not mirrored by the world path", "[colorsplit]")
{
    // Ruling 23. A sphere-cursor stroke splits the facets it crosses, and the sub-facet subdivision is read
    // against each facet's vertex order. The world path's left-handed fix swaps two vertices per facet, so
    // running it on the RAW mesh would reflect the stroke inside every partially painted facet. Splitting the
    // same paint on a plain copy and on a mirrored, anisotropically scaled instance must therefore agree once
    // the world piece is mapped home - the depth runs along z, which neither the mirror nor the x2 touches.
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(40., 40., 20.));   // centred: [-20,20] x [-20,20] x [-10,10]
    TriangleSelector selector(volume->mesh());
    selector.select_patch(2, std::make_unique<TriangleSelector::Sphere>(Vec3f(0.f, 0.f, 10.f), Vec3f(0.f, 0.f, 100.f), 6.f,
                                                                        Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder2, Transform3d::Identity(), true, 0.f);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    const TriangleSelector::TriangleSplittingData paint = volume->mmu_segmentation_facets.get_data();

    // Reference: the same paint, no transform at all.
    ColorSplitResult ref = split_volume_by_paint(volume->mesh().its, paint, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(ref.pieces.size() == 1);
    const double ref_volume = volume_of(ref.pieces[0].second);
    REQUIRE(ref_volume > 0.);

    volume->set_mirror(Vec3d(-1., 1., 1.));
    object->instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    ColorSplitSpace space = color_split_space(*object, *volume);
    REQUIRE(space.world_path);
    REQUIRE(space.to_split.linear().determinant() < 0.);
    ColorSplitResult r = split_volume_by_paint(volume->mesh().its, paint, depths_for_test(1.5), no_cap_no_step(), nullptr, space.to_split);
    REQUIRE(r.pieces.size() == 1);
    TriangleMesh home(r.pieces[0].second);
    home.transform(space.from_split, /*fix_left_handed=*/true);
    REQUIRE(its_volume(home.its) > 0.f);                                   // outward winding survived
    REQUIRE_THAT(double(its_volume(home.its)), WithinRel(ref_volume, 1e-3));

    // And the check has teeth: the contract this replaced - transform the raw mesh, then read the paint -
    // really does move the stroke, so the equality above is not a tautology. MEASURED on this fixture: the
    // swapped vertex order does not merely shift the stroke inside each facet, it leaves nothing the split
    // can build a piece from at all (zero pieces). The assertion stays on "does not reproduce the
    // reference" rather than pinning that count, which is an artefact of the broken path.
    TriangleMesh pre = volume->mesh();
    pre.transform(space.to_split, /*fix_left_handed=*/true);
    ColorSplitResult bad = split_volume_by_paint(pre.its, paint, depths_for_test(1.5), no_cap_no_step(), nullptr);
    double bad_volume = 0.;
    for (const auto &pc : bad.pieces) bad_volume += volume_of(pc.second);
    INFO("old contract: " << bad.pieces.size() << " piece(s), volume " << bad_volume << " vs reference " << ref_volume);
    REQUIRE(std::abs(bad_volume - ref_volume) > 1e-3 * ref_volume);
}

TEST_CASE("colorsplit: a rotated volume on the world path stays in place", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    object.instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    ModelVolume &src = *object.volumes.front();
    src.set_rotation(Vec3d(0.3, 0.2, 0.7));
    src.set_offset(Vec3d(5., -3., 2.));
    object.invalidate_bounding_box();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(space.world_path);
    BoundingBoxf3 before = world_bbox(object);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               depths_for_test(1.5), no_cap_no_step(), nullptr, space.to_split);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
}

TEST_CASE("colorsplit: a rotated anisotropic scale is not mistaken for an isotropic one", "[colorsplit]")
{
    // R S R^T with S = diag(2,1,1) and R carrying x onto the (1,1,1) diagonal: a stretch of 2 along that
    // diagonal, so L = I + a a^T with a = (1,1,1)/sqrt(3) and ALL THREE columns come out sqrt(2) long. Equal
    // column norms alone would therefore send this down the mesh-space path with a single bogus depth scale.
    // (A 45 degree turn about z, the fixture this replaces, only matched the first two columns - the third
    // stayed 1 - so the norm test alone already rejected it and the Gram test was never exercised.)
    // L^T L = I + ones is not s^2 I, and that off-diagonal is the only thing left to decide it.
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    const Matrix3d R = Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitX(), Vec3d(1., 1., 1.).normalized()).toRotationMatrix();
    Transform3d skewed = Transform3d::Identity();
    skewed.linear() = R * Vec3d(2., 1., 1.).asDiagonal() * R.transpose();
    src.set_transformation(skewed);
    ColorSplitSpace space = color_split_space(object, src);
    const Matrix3d L = space.to_split.linear();
    REQUIRE_THAT(L.col(0).norm(), WithinRel(L.col(1).norm(), 1e-9));      // the trap: three equal column norms
    REQUIRE_THAT(L.col(0).norm(), WithinRel(L.col(2).norm(), 1e-9));
    REQUIRE(std::abs(L.col(0).dot(L.col(1))) > 0.5);                      // ... but the columns are not orthogonal
    REQUIRE(space.world_path);                                            // ... so it is still anisotropic
}

TEST_CASE("colorsplit: a degenerate part transformation is refused", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    Transform3d flat = Transform3d::Identity();
    flat.linear() = Vec3d(1., 1., 0.).asDiagonal();                       // the part squashed onto a plane
    src.set_transformation(flat);
    REQUIRE_THROWS_AS(color_split_space(object, src), ColorSplitError);
}

TEST_CASE("colorsplit: the new volumes carry no text, emboss or cut state from the source", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    // The (other, mesh&&) constructor copies all three, and a split part must not stay regenerable from its
    // glyphs or keep being treated as a cut connector.
    src.text_configuration = TextConfiguration{};
    src.emboss_shape       = EmbossShape{};
    src.cut_info           = ModelVolume::CutInfo(CutConnectorType::Plug, 0.5f, 0.5f, /*processed=*/false);
    REQUIRE(src.is_text());
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 0, std::move(r), space, false, false);
    REQUIRE(created.size() == 2);
    for (const ModelVolume *v : created) {
        REQUIRE(!v->text_configuration.has_value());
        REQUIRE(!v->emboss_shape.has_value());
        REQUIRE(!v->is_text());
        REQUIRE(!v->is_svg());
        REQUIRE(!v->cut_info.is_connector);                               // a default CutInfo, not just invalidated
        REQUIRE(v->cut_info.is_processed);
        REQUIRE(v->cut_info.is_from_upper);
    }
}

TEST_CASE("colorsplit: an explicit extruder 0 on the source is an inherit, not a value", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    object.config.set("extruder", 4);
    ModelVolume &src = *object.volumes.front();
    src.config.set("extruder", 0);                                        // "inherit" spelled out
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 0, std::move(r), space, false, false);
    REQUIRE(created.size() == 2);
    REQUIRE(!created[0]->config.has("extruder"));                         // the key is dropped, not copied as 0
    REQUIRE(created[0]->extruder_id() == 4);
    REQUIRE(created[1]->config.extruder() == 2);
}

TEST_CASE("colorsplit: every instance of the object keeps its world bounding box", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelInstance *second = object.add_instance(*object.instances.front());
    second->set_offset(second->get_offset() + Vec3d(80., 25., 0.));
    second->set_rotation(Vec3d(0., 0., 0.6));
    object.invalidate_bounding_box();
    REQUIRE(object.instances.size() == 2);
    const BoundingBoxf3 before0 = object.instance_bounding_box(0, false);
    const BoundingBoxf3 before1 = object.instance_bounding_box(1, false);
    ModelVolume &src = *object.volumes.front();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(!space.world_path);                                           // both instances are unscaled
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    const BoundingBoxf3 after0 = object.instance_bounding_box(0, false);
    const BoundingBoxf3 after1 = object.instance_bounding_box(1, false);
    REQUIRE((after0.min - before0.min).norm() < 1e-3);
    REQUIRE((after0.max - before0.max).norm() < 1e-3);
    REQUIRE((after1.min - before1.min).norm() < 1e-3);
    REQUIRE((after1.max - before1.max).norm() < 1e-3);
}

// ---------------------------------------------------------------------------------------------------------
// Task 8 - spec 8.7 (end-to-end slice parity) and spec 9 S4 (the painted-cube-top wedge). These are the only
// cases here that run the real Print pipeline: they slice the SPLIT object and read back the per-region layer
// geometry, the same "gold standard" the paint-depth suite uses (test_paint_depth_clamp.cpp's file header).

// split_test_config's flows and paint-depth settings with the layer grid pinned uniform, so the layers are a
// plain 0.2mm stack and "the layer just below the surface layer" is exactly print_z = 20 - 0.2.
static DynamicPrintConfig e2e_config()
{
    DynamicPrintConfig c = split_test_config();
    c.option<ConfigOptionFloat>("layer_height")->value               = 0.2;
    c.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.2;
    // Ruling 26: the 2D path notches every OTHER layer by mmu_segmented_region_interlocking_depth so the two
    // colours' teeth interlock - an artefact of segmenting one layer at a time, which the 3D split has no
    // counterpart for (its pieces are solids and the slicer perimeters each of them once). Left at its 0.1
    // default it alternates the 2D claim by i * (40 - 2D + i) = 3.73 mm^2 and swamps the geometric difference
    // the parity case exists to measure, so the comparison zeroes it instead of budgeting for it.
    c.option<ConfigOptionFloat>("mmu_segmented_region_interlocking_depth")->value = 0.;
    return c;
}

// Area (mm^2) `filament` claims on `layer`. A region's filament is its config().wall_filament - which is what
// apply_mm_segmentation stamps on the region it carves for a paint claim (the 2D path, see
// test_paint_depth_clamp.cpp:189-222) and equally what region_config_from_model_volume derives from a
// volume's own "extruder" key (the 3D path, PrintObject.cpp:3244-3257). One number, both paths.
static double filament_area(const Layer *layer, int filament)
{
    double a = 0.;
    for (const LayerRegion *lr : layer->regions())
        if (lr->region().config().wall_filament.value == filament)
            for (const Surface &s : lr->slices.surfaces)
                a += unscaled(unscaled(s.expolygon.area()));
    return a;
}

// Area of every region on the layer, i.e. the whole sliced cross-section.
static double layer_area(const Layer *layer)
{
    double a = 0.;
    for (const LayerRegion *lr : layer->regions())
        for (const Surface &s : lr->slices.surfaces)
            a += unscaled(unscaled(s.expolygon.area()));
    return a;
}

// print.apply + slice, the sequence test_paint_depth_clamp.cpp:97-119 uses. `model` must outlive `print`.
static PrintObject *slice_one(Print &print, Model &model, const DynamicPrintConfig &config)
{
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() > 0);
    return object;
}

// The split exactly as the GUI will drive it (Task 9): depths from the same config the slice will use, the
// space from the object/volume pair, and the RAW mesh with its paint handed over untransformed - `to_split`
// carries the world path instead (Ruling 23). Identity on these fixtures, but the call shape is the point.
static void split_in_place(ModelObject &object, const DynamicPrintConfig &config, const ColorSplitParams &params)
{
    ModelVolume &src = *object.volumes.front();
    const ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(),
                                               scale_depths(color_split_depths(config, {1, 2}), space.depth_scale),
                                               scale_params(params, space.depth_scale), nullptr, space.to_split);
    std::string notes;                                     // a skipped component would make the areas below meaningless
    for (const std::string &w : r.warnings) notes += w + "; ";
    INFO("split warnings: " << notes);
    REQUIRE(r.warnings.empty());
    REQUIRE(apply_color_split(object, 0, std::move(r), space, /*solid_interfaces=*/true, /*keep_base_sparse=*/false).size() == 2);
}

TEST_CASE("colorsplit e2e: split parts slice like the 2D paint-depth claim on a painted side face", "[colorsplit]")
{
    const DynamicPrintConfig config = e2e_config();
    // 2D: the unsplit painted object, whose claim the paint-depth clamp bounds to the band.
    Model painted = painted_model(make_cube(40., 40., 20.), all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2));
    Print        print_2d;
    PrintObject *o2d = slice_one(print_2d, painted, config);
    // 3D: the same paint, split into solid parts first, then sliced through the same pipeline.
    Model split = painted_model(make_cube(40., 40., 20.), all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2));
    split_in_place(*split.objects.front(), config, ColorSplitParams{});
    Print        print_3d;
    PrintObject *o3d = slice_one(print_3d, split, config);
    REQUIRE(o2d->layer_count() == o3d->layer_count());

    // The middle half only: near the top and bottom edges the two paths differ BY DESIGN - the 3D shell
    // tapers along the cube's corner bisector where the painted face meets the caps (spec 3.6), which the 2D
    // segmentation, working one layer at a time with no notion of the faces above or below, cannot express.
    const ColorSplitDepths depths = color_split_depths(config, {1, 2});
    const double D = depths.D, ws = depths.ws;             // 1.40885 / 0.79708 mm from e2e_config()
    const double one_line = 40. * config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value;
    // What each path claims on a middle layer, both derived, neither observed:
    //  * 2D: the +X edge's Voronoi cell is a trapezoid D deep whose ends chamfer at 45 degrees where they meet
    //    the +-Y edges' cells, i.e. D * (40 - D) mm^2.
    //  * 3D (spec 3.4a): the mitred bisector makes the piece exactly D deep perpendicular to the painted face,
    //    but spec 3.6 case B holds the full wall stack before that taper starts, so its ends chamfer from a
    //    depth of ws instead of 0: 40*D - (D - ws)^2. The gap between the two is the **per-vertex case B
    //    corner hold**, ws * (2D - ws) = 1.61 mm^2/layer - a designed deviation bounded by one wall stack
    //    along the vertical creases (the piece is never more than ws wider than the 2D band anywhere, and only
    //    within D of the face). (Before the mitre the piece was only ws + (D - ws)/sqrt(3) = 1.150 mm deep
    //    against the band's 1.409, and fell 8.48 mm^2 short - the other direction, and five times as far.)
    //    Ruling 25 does NOT reach this fixture: a plain cube face has no vertices but its four corners, and
    //    each corner carries a HORIZONTAL boundary edge (against the top or the bottom face) as well as a
    //    vertical one, so its mean n_Q has |n_Q.z| = 0.707 against the patch's 0 - a genuine case B, and the
    //    one "painted side face keeps its full wall stack up to the top edge" pins. The ws ring those corners
    //    earn at the caps is then inherited by the whole +-Y edge between them, since a vertex has one ring
    //    copy. Give the +-Y edges a vertex of their own (make_ringed_box) and it IS a tie, lands on the 2D
    //    45 degree diagonal, and this gap closes there - see the vertical-crease case above.
    const double claim_2d = 40. * D - D * D;
    const double claim_3d = 40. * D - (D - ws) * (D - ws);
    // e2e_config() zeroes the interlocking notch, so every 2D layer claims claim_2d and the whole measured
    // difference is that residual ws * (2D - ws) = 1.61 mm^2/layer - a designed deviation bounded by one wall
    // stack along the vertical creases, which is what the 4.0 mm^2 budget below leaves room for.
    const double bound = 4.0;
    const size_t first = o2d->layer_count() / 4, last = 3 * o2d->layer_count() / 4;
    REQUIRE(first < last);
    double worst = 0., a2_odd = 0., a2_even = 0., a3_seen = 0.;
    for (size_t i = first; i < last; ++i) {
        const double a2 = filament_area(o2d->layers()[i], 2), a3 = filament_area(o3d->layers()[i], 2);
        INFO("layer " << i << " print_z " << o2d->layers()[i]->print_z << ": 2D " << a2 << " mm^2, 3D " << a3
             << " mm^2, diff " << (a2 - a3) << " mm^2 (bound " << bound << ", one outer wall line " << one_line << ")");
        REQUIRE(a2 > 0.);                                  // both paths really claim something ...
        REQUIRE(a3 > 0.);
        REQUIRE_THAT(layer_area(o3d->layers()[i]), WithinRel(40. * 40., 1e-3));   // ... and the parts still tile the cube
        REQUIRE_THAT(a3, WithinAbs(claim_3d, 0.05));       // the 3D piece is D deep perpendicular to the face
        REQUIRE(std::abs(a2 - a3) <= bound);               // ... and within the case B corner hold
        worst = std::max(worst, std::abs(a2 - a3));
        (i % 2 ? a2_odd : a2_even) = a2;                   // equal once the notch is zeroed - it is, below
        a3_seen = a3;
    }
    WARN("parity over layers " << first << ".." << (last - 1) << ": 2D odd " << a2_odd << ", 2D even " << a2_even
         << ", 3D " << a3_seen << " mm^2; worst diff " << worst << " of " << bound << " (2D claim " << claim_2d
         << ", 3D claim " << claim_3d << ", case B corner hold " << (claim_3d - claim_2d)
         << "; D " << D << ", ws " << ws << ", one outer wall line " << one_line << ")");
}

TEST_CASE("colorsplit e2e S4: painted cube top keeps a body outer wall on the side faces", "[colorsplit_spike]")
{
    const DynamicPrintConfig config = e2e_config();
    const ColorSplitDepths   depths = color_split_depths(config, {1, 2});
    // The ring spec 3.6's step is meant to leave the body: one wall stack all the way round the 40mm square.
    // (Taken as the brief states it; the exact ring of an inset square is 4*40*ws - 4*ws^2, 2% less, so the
    // 0.9 factor below keeps its margin either way.)
    const double ws_ring = 4. * 40. * depths.ws;
    double body[2] = {0., 0.};
    for (bool step : {false, true}) {
        Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
        ColorSplitParams params;
        params.crease_step = step;
        params.flat_cap    = false;                        // the cap would set the depth, not the step
        split_in_place(*model.objects.front(), config, params);
        Print        print;
        PrintObject *object = slice_one(print, model, config);
        REQUIRE(object->layer_count() >= 2);
        const size_t idx   = object->layer_count() - 2;    // just below the surface layer
        const Layer *layer = object->layers()[idx];
        body[step ? 1 : 0] = filament_area(layer, 1);
        WARN("S4 crease_step=" << (step ? "on " : "off") << ": body " << body[step ? 1 : 0] << " mm^2, piece "
             << filament_area(layer, 2) << " mm^2 on layer " << idx << " (print_z " << layer->print_z
             << "); one wall stack ring = " << ws_ring << " mm^2 at ws = " << depths.ws << " mm");
        if (step)
            REQUIRE(body[1] >= 0.9 * ws_ring);
    }
    REQUIRE(body[1] > body[0]);                            // the step is what buys the body that outer wall
}
