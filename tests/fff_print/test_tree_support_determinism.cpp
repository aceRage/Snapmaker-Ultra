// Classic tree support must not depend on how many threads it is sliced with.
// docs/superpowers/specs/2026-09-03-slice-determinism.md §7.
//
// TreeSupport::drop_nodes() used to merge nodes and append the next layer's nodes from two
// tbb::parallel_for_each passes in whatever order the workers arrived in, so the branches of a
// classic tree landed in different places on a 20-core machine than on a single core - and in
// different places from one slice to the next on the same machine. These cases slice the same
// object several times under different TBB parallelism caps and require the support geometry to
// come out identical.
//
// Negative control: with src/libslic3r/Support/TreeSupport.cpp reverted to eadc7ec8ed and
// everything else in place, both cases below fail.
//
// Capping TBB with tbb::global_control used to deadlock the very first slice:
// name_tbb_thread_pool_threads_set_locale(), which Print::process() calls once, spawns one task
// per hardware thread and makes each of them wait until all of them are running - which never
// happens under a cap. Thread.cpp now clamps that count to what TBB will actually give.
//
// Deliberately NOT built on Slic3r::Test::init_print(): that fixture still uses PrusaSlicer's
// config key names and throws on this fork ("Unknown option exception: first_layer_extrusion_width",
// see the spec §4.0). Everything here goes through DynamicPrintConfig::full_print_config(), the
// same route tests/fff_print/test_support_groups.cpp takes.

#include <catch2/catch.hpp>

#include <sstream>
#include <string>

#include <tbb/global_control.h>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

void add_box(ModelObject *object, double x0, double y0, double z0, double dx, double dy, double dz)
{
    TriangleMesh box = make_cube(dx, dy, dz);
    box.translate(float(x0), float(y0), float(z0));
    object->add_volume(box);
}

// The corpus' twopart_bridge shape - an off-centre pillar carrying a two-piece deck that overhangs
// it further on one side than the other - scaled up. See tests/data/support_corpus/make_fixtures.py
// for the original boxes.
//
// Two properties matter and both were learned the hard way. The overhang is ASYMMETRIC: a
// symmetric cap resolves its merge ties the same way however the work is split, so it never
// catches the bug. And the deck is BIG: at the corpus' 34 x 16 mm the layer holds only a few dozen
// contact nodes, too few for tbb::parallel_for_each to split, so the old code ran the passes on one
// thread whatever the cap said and came out identical. Here it is 80 x 50 mm.
//
// Everything is one connected solid resting on the bed, so each layer has one island and the print
// order of the objects cannot vary.
void add_bridge(Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "twopart_bridge";
    add_box(object, -5., -15., 0., 10., 30., 16.);   // pillar
    add_box(object, -40., -25., 16., 45., 50., 2.);  // deck, long side
    add_box(object, 5., -25., 16., 35., 50., 2.);    // deck, short side
    object->add_instance();
    object->ensure_on_bed();
}

// Everything the support generator produced, as text: the layer's print_z and every point of every
// support extrusion on it. Comparing this rather than the G-code keeps the case about the support
// generator and not about the writer. The sizes come back too, so a fixture that quietly stops
// generating support fails on those instead of passing on two identical empty digests.
struct SupportDigest
{
    std::string text;
    size_t      layers = 0;
    size_t      points = 0;

    bool operator==(const SupportDigest &other) const { return text == other.text; }
};

SupportDigest support_digest(const Print &print)
{
    SupportDigest      digest;
    std::ostringstream out;
    for (const PrintObject *object : print.objects()) {
        for (const SupportLayer *layer : object->support_layers()) {
            ++digest.layers;
            out << "z " << layer->print_z << " islands " << layer->support_islands.size() << "\n";
            for (const Polyline &pl : layer->support_fills.as_polylines()) {
                digest.points += pl.points.size();
                out << " p";
                for (const Point &pt : pl.points)
                    out << ' ' << pt.x() << ',' << pt.y();
                out << "\n";
            }
        }
    }
    digest.text = out.str();
    return digest;
}

// One slice of the bridge with classic tree support, under a TBB parallelism cap.
SupportDigest slice_with_threads(size_t max_threads)
{
    tbb::global_control gc(tbb::global_control::max_allowed_parallelism, max_threads);

    Print print;
    Model model;
    print.set_status_silent();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", "1"},
        {"support_type", "tree(auto)"},
        {"support_style", "tree_slim"},
        // Half the default 5 mm, so this deck carries roughly a thousand contact nodes per layer -
        // enough for the workers to really share them. See add_bridge().
        {"tree_support_branch_distance", "2"},
        // Classic Arachne-free walls, so a difference here can only come from the support path.
        {"wall_generator", "classic"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    add_bridge(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.process();

    return support_digest(print);
}

// A tree under this deck is thousands of extrusion points over dozens of layers. If the fixture
// ever stops producing support - a renamed config key, a changed default - the digests would agree
// trivially and the case would pass while testing nothing, so pin the size first.
void require_real_support(const SupportDigest &digest)
{
    INFO("support layers " << digest.layers << ", extrusion points " << digest.points);
    REQUIRE(digest.layers > 20);
    REQUIRE(digest.points > 2000);
}

} // namespace

TEST_CASE("classic tree support is identical on one thread and on many", "[TreeSupportDeterminism]")
{
    const SupportDigest one  = slice_with_threads(1);
    const SupportDigest many = slice_with_threads(32);

    require_real_support(one);
    CHECK(one == many);
}

TEST_CASE("classic tree support is identical across several thread counts", "[TreeSupportDeterminism]")
{
    const SupportDigest reference = slice_with_threads(1);
    require_real_support(reference);

    for (size_t threads : {size_t(2), size_t(4), size_t(20)}) {
        INFO("max_allowed_parallelism = " << threads);
        CHECK(slice_with_threads(threads) == reference);
    }
}
