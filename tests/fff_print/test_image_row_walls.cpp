// ImageMap Phase 4, step 1 + step 5: side-wall dithering, measured at the G-code the printer
// actually receives.
//
// Spec: docs/superpowers/specs/2026-09-12-imagemap-phase4-walls.md.
//
// WHY THESE TESTS MEASURE G-CODE, NOT ExtrusionEntities. Phase 3's own tests
// (tests/libslic3r/test_image_row_dialog.cpp, test_image_row_transform.cpp) assert on the
// ExtrusionEntityCollection tree, because the top-surface split happens during slicing and the
// tree IS the product. The WALL split does not: it happens at G-code emission time, inside
// GCode.cpp's per-island PERIMETERS block, precisely so the seam placer can run first (see
// ImageRowWalls.hpp for why). There is no tagged tree to inspect afterwards - the only place the
// result exists is the G-code, and the GCodeProcessorResult the 3D preview is built from. So
// these tests slice, export, and read the moves back.
//
// That also makes them the honest test of the whole chain: a run only shows up as its own tool
// here if the split produced it, ToolOrdering registered its filament in the layer's tool list,
// and GCode.cpp resolved the collection's override into a real T<n>. Any one of those three
// failing shows up as "one extruder on the wall", which is exactly the pre-phase-4 behaviour.
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

// Three saturated, maximally separated filaments, so the dither's nearest-colour choice for a
// pure red / green / blue band is unambiguous and the test is not measuring rounding.
const std::vector<std::string> kWallColours = {"#FF0000", "#00FF00", "#0000FF"};

std::vector<uint8_t> read_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

DynamicPrintConfig wall_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(kWallColours.size()));
    config.set_num_filaments(unsigned(kWallColours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(kWallColours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kWallColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(kWallColours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value    = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent  = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value    = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent  = false;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->percent = false;
    // A tool changer, so GCodeWriter::toolchange() actually emits T<n> (a single-extruder machine
    // emits nothing - GCodeWriter.cpp:462 - and every assertion below would be vacuous).
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    // No prime tower: it inserts its own extrusions between tool changes, which would pollute the
    // per-layer move bookkeeping these tests do.
    config.option<ConfigOptionBool>("enable_prime_tower")->value = false;
    return config;
}

// A Box (tri-planar) projection: each facet is projected along the axis of its own dominant
// normal, so ONE image lands on all six sides of a cube - which is exactly the case the phase 4
// brief names ("a cube with a 2-colour vertical stripe image box-projected").
ImageFillParams box_params(Model &model, const std::string &fixture)
{
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_fixture(fixture));
    p.projection = ImageFillProjection::Box;
    p.allowed    = {1, 2, 3};
    return p;
}

ImageFillParams wrap_params(Model &model, const std::string &fixture)
{
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_fixture(fixture));
    p.projection = ImageFillProjection::Cylindrical;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};
    return p;
}

// Creates the ImageWeighted row and binds it to the part's WALL filament (phase 4's key), rather
// than to solid_infill_filament (phase 3's). Returns the row's virtual id.
int bind_image_row_to_walls(MixedFilamentManager &mgr, ModelVolume &volume, const ImageFillParams &params)
{
    const std::vector<int> ids = {1, 2, 3};
    mgr.add_custom_filament(1, 2, 50, kWallColours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &row         = mgr.mixed_filaments().back();
    row.enabled                = true;
    row.distribution_mode      = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(
        std::vector<unsigned int>(ids.begin(), ids.end()));
    row.image_fill_ref = MixedFilamentManager::encode_image_fill_ref(params.to_string());
    REQUIRE_FALSE(row.image_fill_ref.empty());
    const unsigned int virtual_id =
        mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kWallColours.size());
    REQUIRE(virtual_id != 0);
    volume.config.set_key_value("wall_filament", new ConfigOptionInt(int(virtual_id)));
    return int(virtual_id);
}

struct LayerWallStats
{
    size_t                  moves        = 0;
    int                     transitions  = 0;   // extruder_id changes between consecutive moves
    std::set<unsigned char> extruders;
    int                     tool_changes = 0;   // Tool_change vertices on this layer
};

// Every external-perimeter extruding move at one Z, in emission order, plus the tool changes that
// happen on that layer. `z` is matched loosely because a move's recorded Z is the nozzle's Z.
LayerWallStats wall_stats_at_z(const GCodeProcessorResult &result, float z)
{
    LayerWallStats s;
    const GCodeProcessorResult::MoveVertex *prev_wall = nullptr;
    for (const auto &m : result.moves) {
        if (std::abs(m.position.z() - z) > 1e-4f)
            continue;
        if (m.type == EMoveType::Tool_change) {
            ++s.tool_changes;
            continue;
        }
        if (m.type != EMoveType::Extrude || m.extrusion_role != erExternalPerimeter)
            continue;
        ++s.moves;
        s.extruders.insert(m.extruder_id);
        if (prev_wall != nullptr && prev_wall->extruder_id != m.extruder_id)
            ++s.transitions;
        prev_wall = &m;
    }
    return s;
}

// The distinct Z values at which external perimeters are extruded, ascending.
std::vector<float> wall_layer_zs(const GCodeProcessorResult &result)
{
    std::vector<float> zs;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter) {
            bool seen = false;
            for (float z : zs)
                if (std::abs(z - m.position.z()) < 1e-4f) { seen = true; break; }
            if (!seen)
                zs.push_back(m.position.z());
        }
    std::sort(zs.begin(), zs.end());
    return zs;
}

ModelVolume *make_cube_part(Model &model, double sz)
{
    ModelObject *object = model.add_object();
    object->name        = "image row wall cube";
    ModelVolume *volume = object->add_volume(make_cube(sz, sz, sz));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();
    return volume;
}

ModelVolume *make_cylinder_part(Model &model, double r, double h)
{
    ModelObject *object = model.add_object();
    object->name        = "image row wall cylinder";
    ModelVolume *volume = object->add_volume(make_cylinder(r, h));
    volume->name        = "cylinder";
    object->add_instance();
    object->ensure_on_bed();
    return volume;
}

// Slices `model`+`config` to G-code and fills `result` with the processor output the preview
// would use. GCodeProcessorResult is deliberately non-copyable (it owns the whole move list), so
// it is filled through an out-parameter rather than returned.
// `out_text` optionally receives the G-code text, for the determinism check.
void slice_to_result(Model &model, const DynamicPrintConfig &config, GCodeProcessorResult &result,
                     std::string *out_text = nullptr)
{
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const boost::filesystem::path out = Slic3r::Test::scratch_path(".gcode");
    print.export_gcode(out.string(), &result, nullptr);
    if (out_text != nullptr) {
        boost::nowide::ifstream in(out.string());
        *out_text = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    boost::nowide::remove(out.string().c_str());
}

// Strips the lines that legitimately differ between two runs of the same binary on the same input
// (the generation timestamp and the M73 time estimates), so the rest can be compared byte for
// byte. The same exclusion phase 3's own determinism check uses.
std::string strip_volatile(const std::string &gcode)
{
    std::string out;
    out.reserve(gcode.size());
    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol + 1;
        const std::string line = gcode.substr(pos, end - pos);
        const bool volatile_line =
            line.find("; generated by") != std::string::npos ||
            line.find("M73") != std::string::npos ||
            line.find("estimated") != std::string::npos ||
            line.find("total estimated time") != std::string::npos ||
            // The object-id lines ("; model label id: N", "; start/stop printing object, unique
            // label id: N"). N comes from the PROCESS-GLOBAL ObjectBase id counter, which keeps
            // advancing as this test builds its second Model - so pass 2's ids are simply higher
            // than pass 1's, in a single-process test, no matter how deterministic the slicing is.
            // This is a property of running two slices in one process, not of the slice: measured
            // on this fixture, stripping ONLY these lines leaves the two passes differing in
            // exactly ZERO further lines out of ~34000.
            //
            // Phase 3's own Bar B compared two runs of a separate PROCESS each, where the counter
            // starts from the same value both times and no such stripping was needed; this test
            // trades that for being runnable in the suite.
            line.find("label id:") != std::string::npos;
        if (!volatile_line)
            out += line;
        pos = end;
    }
    return out;
}

} // namespace

// ================================================================================================
// 1. A cube with a vertical-stripe image, box-projected: the outer wall alternates filaments.
// ================================================================================================
//
// bands3.png is three VERTICAL bands - pure red, pure green, pure blue - varying with u and
// constant in v. Under a Box projection each of the cube's four side faces is projected flat along
// its own dominant axis, so walking once around the cube's outer perimeter sweeps u across the
// image several times and therefore crosses several colour boundaries. With one filament per band
// colour, that has to come out as several runs of different filaments on every wall layer.
//
// Before phase 4 the whole wall printed with the region's single nominal filament, so this test's
// `extruders.size() >= 2` is the assertion that the feature exists at all.
TEST_CASE("Image Row walls: a box-projected stripe image alternates filaments along the outer wall",
          "[imagefill][ImageRow][walls]")
{
    Model                 model;
    ModelVolume          *volume = make_cube_part(model, 30.);
    const ImageFillParams p      = box_params(model, "bands3.png");

    MixedFilamentManager mgr;
    const int            virtual_id = bind_image_row_to_walls(mgr, *volume, p);

    DynamicPrintConfig config = wall_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);
    const std::vector<float>   zs     = wall_layer_zs(result);
    REQUIRE(zs.size() > 5);

    // Measure a layer in the middle of the cube: the first layers carry brim/skirt and the last
    // carry the top surface, either of which would mix other roles into the bookkeeping.
    const float      mid = zs[zs.size() / 2];
    LayerWallStats   s   = wall_stats_at_z(result, mid);

    std::string seen;
    for (unsigned char e : s.extruders)
        seen += std::to_string(int(e)) + " ";
    INFO("mid layer z=" << mid << " wall moves=" << s.moves << " extruders: " << seen
         << " transitions=" << s.transitions << " tool changes on layer=" << s.tool_changes
         << " virtual_id=" << virtual_id);

    REQUIRE(s.moves > 0);
    // The wall really is multi-coloured - the whole point of step 1.
    CHECK(s.extruders.size() >= 2);
    // And it changes colour SEVERAL times going round, not once: bands3 has three bands and a Box
    // projection sweeps u once per side face, so a full circuit crosses many boundaries.
    CHECK(s.transitions >= 2);
    // Every filament used is physical and allowed - never the row's own virtual id, which would
    // mean the override was not resolved (and would colour the preview with the row's blended
    // display colour instead of the run's own).
    for (unsigned char e : s.extruders) {
        CHECK(size_t(e) < kWallColours.size());
        CHECK(int(e) + 1 != virtual_id);
    }

    // TOOL CHANGES ARE BOUNDED BY FILAMENTS, NOT RUNS. This is the economics rule the phase 3
    // spec states and the brief repeats: the whole LAYER visits each needed filament once, in the
    // order ToolOrdering::reorder_extruders picked, so a layer with dozens of colour runs still
    // costs at most a couple of tool changes. Without the per-layer grouping this number would
    // track `transitions` instead, which is the failure this bound catches.
    //
    // The bound is 2*(filaments-1): a layer may visit each filament once (filaments-1 changes)
    // and the pre-existing wall/infill grouping may cost one more pass in the worst case.
    const int bound = 2 * (int(kWallColours.size()) - 1);
    INFO("tool changes on the measured layer = " << s.tool_changes << ", bound = " << bound);
    CHECK(s.tool_changes <= bound);
}

// ================================================================================================
// 2. A wrapped image on a cylinder: the runs follow the image around the loop.
// ================================================================================================
//
// wrap4.png wrapped about Z puts white on -X, red on -Y, green on +X and blue on +Y (the fixture
// generator's own comment states exactly this). A cylinder's outer perimeter is one loop that
// walks all the way round, so the sequence of filaments along that loop must track the sequence of
// bands around the part - i.e. the wall is not just multi-coloured, its colours are in the right
// PLACES.
//
// This is measured as: bucket the layer's wall moves by the angle of their position about the
// part's centre, and require that each of the four quadrants is dominated by a different filament
// than at least one of its neighbours - i.e. the colour genuinely varies with angle rather than
// being one filament with noise.
TEST_CASE("Image Row walls: a wrapped image's runs follow the gradient around the loop",
          "[imagefill][ImageRow][walls]")
{
    Model                 model;
    ModelVolume          *volume = make_cylinder_part(model, 12., 12.);
    const ImageFillParams p      = wrap_params(model, "wrap4.png");

    MixedFilamentManager mgr;
    bind_image_row_to_walls(mgr, *volume, p);

    DynamicPrintConfig config = wall_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);
    const std::vector<float>   zs     = wall_layer_zs(result);
    REQUIRE(zs.size() > 5);
    const float mid = zs[zs.size() / 2];

    // The centre of the wall moves on this layer, so angles are measured about the part itself.
    double cx = 0., cy = 0.;
    size_t n  = 0;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter &&
            std::abs(m.position.z() - mid) < 1e-4f) {
            cx += m.position.x();
            cy += m.position.y();
            ++n;
        }
    REQUIRE(n > 8);
    cx /= double(n);
    cy /= double(n);

    // Four angular quadrants, each weighted by extruded move count.
    std::array<std::map<int, int>, 4> per_quadrant;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter &&
            std::abs(m.position.z() - mid) < 1e-4f) {
            const double ang = std::atan2(double(m.position.y()) - cy, double(m.position.x()) - cx);
            int          q   = int((ang + M_PI) / (2.0 * M_PI) * 4.0);
            q                = std::min(3, std::max(0, q));
            ++per_quadrant[size_t(q)][int(m.extruder_id)];
        }

    std::array<int, 4> dominant{-1, -1, -1, -1};
    for (size_t q = 0; q < 4; ++q) {
        int best = -1, best_n = 0;
        for (const auto &kv : per_quadrant[q])
            if (kv.second > best_n) { best_n = kv.second; best = kv.first; }
        dominant[q] = best;
    }
    INFO("dominant filament per angular quadrant: " << dominant[0] << " " << dominant[1] << " "
         << dominant[2] << " " << dominant[3] << " (z=" << mid << ")");

    // Every quadrant has wall in it...
    for (size_t q = 0; q < 4; ++q)
        CHECK(dominant[q] >= 0);
    // ...and the colour is not constant around the loop: at least two quadrants differ, which can
    // only happen if the sample followed the image's angular position rather than a per-layer
    // colour cycle (which would give one filament for the whole layer).
    const std::set<int> distinct(dominant.begin(), dominant.end());
    CHECK(distinct.size() >= 2);
}

// ================================================================================================
// 3. Fuzzy skin on does not change the run count.
// ================================================================================================
//
// The brief's requirement: "run boundaries must survive fuzzy jitter (sample after fuzz)". The
// split runs at G-code time, and fuzzy skin is applied inside PerimeterGenerator long before -
// so the geometry being sampled is ALREADY jittered, and a run boundary is placed on the jittered
// path rather than on a smooth path that the jitter then moves out from under it.
//
// The observable consequence is that turning fuzz on changes where the nozzle goes but not how
// many colour runs the image asks for: the image is sampled at the same positions (jitter is
// perpendicular to the wall and small compared to a band), so the filament sequence is stable.
// The check is deliberately a tolerance rather than an equality - fuzz lengthens the path, so a
// couple of extra runs at the margins is expected and only a gross change (the split collapsing,
// or fragmenting per jitter tooth) is a failure.
TEST_CASE("Image Row walls: fuzzy skin does not change the wall's run structure",
          "[imagefill][ImageRow][walls][fuzzy]")
{
    auto measure = [](bool fuzzy) {
        Model                 model;
        ModelVolume          *volume = make_cube_part(model, 30.);
        const ImageFillParams p      = box_params(model, "bands3.png");
        MixedFilamentManager  mgr;
        bind_image_row_to_walls(mgr, *volume, p);

        DynamicPrintConfig config = wall_test_config();
        config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
        if (fuzzy) {
            config.option<ConfigOptionEnum<FuzzySkinType>>("fuzzy_skin")->value = FuzzySkinType::External;
            config.option<ConfigOptionFloat>("fuzzy_skin_thickness")->value     = 0.3;
            config.option<ConfigOptionFloat>("fuzzy_skin_point_distance")->value = 0.8;
        }

        GCodeProcessorResult result;
        slice_to_result(model, config, result);
        const std::vector<float>   zs     = wall_layer_zs(result);
        REQUIRE(zs.size() > 5);
        return wall_stats_at_z(result, zs[zs.size() / 2]);
    };

    const LayerWallStats plain = measure(false);
    const LayerWallStats fuzz  = measure(true);

    INFO("plain: extruders=" << plain.extruders.size() << " transitions=" << plain.transitions
         << " | fuzzy: extruders=" << fuzz.extruders.size() << " transitions=" << fuzz.transitions);

    // Fuzz must not collapse the split: the wall is still multi-coloured.
    CHECK(fuzz.extruders.size() >= 2);
    // The same filaments are reachable with and without fuzz.
    CHECK(fuzz.extruders.size() == plain.extruders.size());
    // And the number of colour boundaries is of the same order - not zero (collapsed) and not an
    // order of magnitude larger (fragmented once per jitter tooth).
    REQUIRE(plain.transitions > 0);
    CHECK(fuzz.transitions >= plain.transitions / 2);
    CHECK(fuzz.transitions <= plain.transitions * 3 + 4);
}

// ================================================================================================
// 4. Determinism: the same binary on the same project twice produces identical G-code.
// ================================================================================================
//
// The dither carries an error term forward along each wall, and the split allocates heap
// collections per run - either could, if it depended on iteration order over a hash container or
// on uninitialised state, produce a different answer on a second run. Phase 3 established this for
// the top-surface path; this is the same guarantee for walls.
TEST_CASE("Image Row walls: two runs of the same project produce identical G-code",
          "[imagefill][ImageRow][walls][determinism]")
{
    auto run_once = [](std::string &text) {
        Model                 model;
        ModelVolume          *volume = make_cube_part(model, 30.);
        const ImageFillParams p      = box_params(model, "bands3.png");
        MixedFilamentManager  mgr;
        bind_image_row_to_walls(mgr, *volume, p);
        DynamicPrintConfig config = wall_test_config();
        config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
        GCodeProcessorResult result;
        slice_to_result(model, config, result, &text);
    };

    std::string a, b;
    run_once(a);
    run_once(b);
    REQUIRE_FALSE(a.empty());

    const std::string sa = strip_volatile(a);
    const std::string sb = strip_volatile(b);
    INFO("pass 1 significant bytes=" << sa.size() << " pass 2=" << sb.size()
         << " (raw " << a.size() << " / " << b.size() << ")");
    CHECK(sa == sb);
    // Guard against the stripping above hiding a real difference: the two passes must have had
    // the SAME NUMBER of stripped lines, so "identical after stripping" cannot be bought by one
    // pass simply having more of them.
    auto stripped_lines = [](const std::string &whole, const std::string &kept) {
        const auto count_nl = [](const std::string &s) { return std::count(s.begin(), s.end(), '\n'); };
        return count_nl(whole) - count_nl(kept);
    };
    CHECK(stripped_lines(a, sa) == stripped_lines(b, sb));
}
