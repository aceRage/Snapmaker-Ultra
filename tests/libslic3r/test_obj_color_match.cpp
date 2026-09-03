#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "libslic3r/ObjColorMatch.hpp"

using namespace Slic3r;

static RGBA rgb(float r, float g, float b) { return RGBA{r, g, b, 1.f}; }

// A deliberately boring metric for the policy tests: the largest channel difference, scaled so the
// production tolerance (20) means "within 0.2 of a channel". Injecting it keeps these tests about
// the reuse / add / merge decisions rather than about colour science.
static float channel_distance(const RGBA &a, const RGBA &b)
{
    return 100.f * std::max(std::max(std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1])), std::fabs(a[2] - b[2]));
}

// Every result has to satisfy these, whatever went in.
static void check_invariants(const ObjColorMatchResult &r, size_t inputs, size_t existing, size_t max_slots)
{
    REQUIRE(r.filament_ids.size() == inputs);
    REQUIRE(r.input == inputs);
    REQUIRE(r.clusters >= 1);
    REQUIRE(r.reused + r.added + r.merged == r.clusters);
    REQUIRE(existing + r.added_colors.size() <= max_slots);
    REQUIRE(r.added == r.added_colors.size());
    const size_t slots = existing + r.added_colors.size();
    for (unsigned char id : r.filament_ids) {
        REQUIRE(id >= 1);
        REQUIRE((size_t) id <= slots);
    }
    REQUIRE(r.first_extruder_id >= 1);
    REQUIRE((size_t) r.first_extruder_id <= slots);
}

SCENARIO("Headless colour import maps colours onto filament slots", "[objcolor]")
{
    GIVEN("an imported colour that is nearly a loaded filament")
    {
        const std::vector<RGBA> existing{rgb(1.f, 0.f, 0.f), rgb(0.f, 0.f, 1.f)};
        const std::vector<RGBA> input(6, rgb(0.95f, 0.02f, 0.02f));   // within 0.2 of the red
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, false, existing, r, 16, 20.f, &channel_distance));
        THEN("the loaded spool is reused instead of burning a slot")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(r.reused == 1);
            REQUIRE(r.added == 0);
            REQUIRE(r.added_colors.empty());
            for (unsigned char id : r.filament_ids)
                REQUIRE(id == 1);
            REQUIRE(r.first_extruder_id == 1);
        }
    }

    GIVEN("an imported colour nothing loaded is close to, and room to spare")
    {
        const std::vector<RGBA> existing{rgb(1.f, 0.5f, 0.f)};        // orange
        const std::vector<RGBA> input(4, rgb(0.f, 0.f, 1.f));         // blue
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, false, existing, r, 16, 20.f, &channel_distance));
        THEN("a new slot is asked for, carrying the imported colour")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(r.added == 1);
            REQUIRE(r.reused == 0);
            REQUIRE(r.added_colors.size() == 1);
            REQUIRE(r.added_colors[0][2] == Approx(1.f).margin(0.02));
            for (unsigned char id : r.filament_ids)
                REQUIRE(id == 2);                                      // the slot just added
        }
    }

    GIVEN("all sixteen slots already taken")
    {
        std::vector<RGBA> existing;
        for (int i = 0; i < 16; ++i)
            existing.push_back(rgb(float(i) / 15.f, 0.f, 0.f));        // a ramp of reds
        const std::vector<RGBA> input(3, rgb(0.f, 1.f, 0.f));          // a green, far from all of them
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, false, existing, r, 16, 20.f, &channel_distance));
        THEN("nothing is added and the colour merges into its nearest slot")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(r.added == 0);
            REQUIRE(r.added_colors.empty());
            REQUIRE(r.merged == 1);
            // By max-channel every red is 1.0 away in green, so the first one wins the tie.
            for (unsigned char id : r.filament_ids)
                REQUIRE(id == 1);
        }
    }

    GIVEN("far more distinct colours than there are slots")
    {
        std::vector<RGBA> input;
        for (int i = 0; i < 40; ++i) {
            const float t = float(i) / 39.f;
            input.push_back(rgb(t, 1.f - t, float((i * 7) % 5) / 4.f));
        }
        const std::vector<RGBA> existing{rgb(1.f, 0.5f, 0.f), rgb(1.f, 1.f, 1.f), rgb(0.f, 0.f, 0.f)};
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, false, existing, r, 16, 20.f));
        THEN("the sixteen-slot limit holds and every colour still has a slot")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(existing.size() + r.added_colors.size() <= 16);
        }
    }

    GIVEN("a single-colour import")
    {
        const std::vector<RGBA> existing{rgb(1.f, 0.5f, 0.f)};
        const std::vector<RGBA> input{rgb(0.f, 0.f, 1.f), rgb(0.f, 0.f, 0.9f), rgb(0.1f, 0.f, 1.f)};
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, true, existing, r, 16, 20.f, &channel_distance));
        THEN("it is one cluster and one slot, whatever the individual colours say")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(r.clusters == 1);
            REQUIRE(r.added == 1);
            for (unsigned char id : r.filament_ids)
                REQUIRE(id == r.filament_ids.front());
        }
    }

    GIVEN("exactly one filament loaded")
    {
        const std::vector<RGBA> existing{rgb(1.f, 1.f, 1.f)};
        const std::vector<RGBA> input{rgb(1.f, 0.f, 0.f), rgb(1.f, 0.f, 0.f), rgb(0.f, 1.f, 0.f),
                                      rgb(0.f, 1.f, 0.f), rgb(0.f, 0.f, 1.f), rgb(0.f, 0.f, 1.f)};
        ObjColorMatchResult     r;
        REQUIRE(obj_color_auto_match(input, false, existing, r, 16, 20.f, &channel_distance));
        THEN("slots are added for the imported colours")
        {
            check_invariants(r, input.size(), existing.size(), 16);
            REQUIRE(r.added >= 1);
            REQUIRE(r.added_colors.size() == r.added);
        }
    }

    GIVEN("no input colours at all")
    {
        ObjColorMatchResult r;
        THEN("there is nothing to match")
        {
            REQUIRE_FALSE(obj_color_auto_match({}, false, {rgb(1.f, 1.f, 1.f)}, r, 16, 20.f));
            REQUIRE(r.filament_ids.empty());
        }
    }
}

SCENARIO("The headless colour distance is in real CIE units", "[objcolor]")
{
    // The tolerance is only defensible if its number means something, so pin the pairs that chose
    // OBJ_COLOR_MATCH_TOLERANCE.
    GIVEN("colours nobody would load side by side")
    {
        THEN("they fall well inside the tolerance")
        {
            // two oranges, and pure red against a loaded dark red
            REQUIRE(obj_color_distance(rgb(1.f, 0.533f, 0.f), rgb(1.f, 0.6f, 0.f)) < 10.f);
            REQUIRE(obj_color_distance(rgb(1.f, 0.f, 0.f), rgb(0.835f, 0.f, 0.f)) == Approx(15.9f).margin(1.0f));
            REQUIRE(obj_color_distance(rgb(1.f, 0.f, 0.f), rgb(0.835f, 0.f, 0.f)) < OBJ_COLOR_MATCH_TOLERANCE);
        }
    }
    GIVEN("genuinely different hues")
    {
        THEN("they are far outside the tolerance")
        {
            REQUIRE(obj_color_distance(rgb(0.f, 1.f, 0.f), rgb(0.996f, 0.776f, 0.f)) > 4.f * OBJ_COLOR_MATCH_TOLERANCE);
            REQUIRE(obj_color_distance(rgb(0.f, 0.f, 1.f), rgb(0.835f, 0.f, 0.f)) > 4.f * OBJ_COLOR_MATCH_TOLERANCE);
        }
    }
    GIVEN("the same colour twice")
    {
        THEN("the distance is zero")
        {
            REQUIRE(obj_color_distance(rgb(0.2f, 0.4f, 0.6f), rgb(0.2f, 0.4f, 0.6f)) == Approx(0.f).margin(1e-4));
        }
    }
}
