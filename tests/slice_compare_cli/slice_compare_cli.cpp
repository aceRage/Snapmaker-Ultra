// Ultra (support groups): a headless front end for the fork's own Slice Compare engine, so a
// script can apply its criteria to two G-code files.
//
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md §3.7 originally gated the support
// group work on byte-identical G-code. The plan's own §5.1 experiment showed the slicer is not
// reliably reproducible run to run (see the Stage 2 status section), so the gate became the
// tolerance the plan already named as its "second, cheaper control": zero changed config rows,
// every layer identical, and 100 % of segments matching on every matched layer.
//
// This lives under tests/ on purpose. Everything it calls is already headless libslic3r
// (SliceCompare::load_snapshot_from_file and the diff_* functions), so no hook into the shipped
// application was needed; building it as a test target keeps it out of the product entirely.
//
//     slice_compare_cli <baseline.gcode> <candidate.gcode> [--segments-of N] [--quiet]
//
// Prints one JSON object on stdout and exits 0 when the two files meet the criteria, 1 when they
// do not, 2 on a load error.

// nanosvg is a header-only dependency whose implementation the application compiles inside
// src/slic3r/GUI/BitmapCache.cpp. libslic3r references it (svg.cpp, NSVGUtils.cpp), so a non-GUI
// executable linking libslic3r has to supply the implementation itself or the link fails on
// nsvgParse / nsvgDelete. Three lines here, and no change to the shipped application.
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/SliceCompare/Diff.hpp"
#include "libslic3r/SliceCompare/Snapshot.hpp"

using namespace Slic3r;
using namespace Slic3r::SliceCompare;

namespace {

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else
                out += c;
        }
    }
    return out;
}

int fail(const std::string &message)
{
    std::cout << "{\"ok\": false, \"error\": \"" << json_escape(message) << "\"}" << std::endl;
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    std::string a_path, b_path;
    // How many matched layers to run the (much more expensive) segment comparison over. 0 = all.
    int  segments_of = 0;
    bool quiet       = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--segments-of" && i + 1 < argc)
            segments_of = std::atoi(argv[++i]);
        else if (arg == "--quiet")
            quiet = true;
        else if (a_path.empty())
            a_path = arg;
        else if (b_path.empty())
            b_path = arg;
    }
    if (a_path.empty() || b_path.empty()) {
        std::cerr << "usage: slice_compare_cli <baseline.gcode> <candidate.gcode> "
                     "[--segments-of N] [--quiet]\n";
        return 2;
    }

    FileLoadResult ra = load_snapshot_from_file(a_path);
    if (! ra.snapshot)
        return fail("baseline: " + (ra.error.empty() ? std::string("could not load") : ra.error));
    FileLoadResult rb = load_snapshot_from_file(b_path);
    if (! rb.snapshot)
        return fail("candidate: " + (rb.error.empty() ? std::string("could not load") : rb.error));

    const Snapshot &a = *ra.snapshot;
    const Snapshot &b = *rb.snapshot;

    // 1) Config rows. diff_configs already drops the volatile keys.
    const std::vector<ConfigRow> config_rows = diff_configs(a, b);

    // 2) Layers.
    const LayerDiff layers = diff_layers(a, b);

    // 3) Segments, over the matched layers. A layer counts as clean only when every segment on
    //    both sides found an exact counterpart - `jitter` is a near-miss rescue, so it is NOT
    //    treated as a match here: a moved extrusion is exactly what this gate must catch.
    long long seg_both = 0, seg_a_only = 0, seg_b_only = 0, seg_jitter = 0;
    int       seg_layers_checked = 0, seg_layers_dirty = 0;
    std::vector<int> dirty_layers;
    for (const LayerMatch &m : layers.rows) {
        if (m.zkey_a < 0 || m.zkey_b < 0)
            continue;
        if (segments_of > 0 && seg_layers_checked >= segments_of)
            break;
        auto ia = a.layers.find(m.zkey_a);
        auto ib = b.layers.find(m.zkey_b);
        if (ia == a.layers.end() || ib == b.layers.end())
            continue;
        const SegDiff sd = diff_segments(ia->second, ib->second);
        seg_both   += (long long) sd.both.size();
        seg_a_only += (long long) sd.a_only.size();
        seg_b_only += (long long) sd.b_only.size();
        seg_jitter += (long long) sd.jitter.size();
        ++ seg_layers_checked;
        if (! sd.a_only.empty() || ! sd.b_only.empty() || ! sd.jitter.empty()) {
            ++ seg_layers_dirty;
            if (dirty_layers.size() < 20)
                dirty_layers.push_back(m.zkey_a);
        }
    }
    const long long seg_total   = seg_both + seg_a_only + seg_b_only + seg_jitter;
    const double    seg_percent = seg_total == 0 ? 100.0 : 100.0 * double(seg_both) / double(seg_total);

    const bool ok = config_rows.empty() && layers.changed == 0 && layers.a_only == 0 &&
                    layers.b_only == 0 && seg_layers_dirty == 0;

    std::cout << "{";
    std::cout << "\"ok\": " << (ok ? "true" : "false");
    std::cout << ", \"config_rows\": " << config_rows.size();
    std::cout << ", \"layers\": {\"matched\": " << layers.matched
              << ", \"identical\": " << layers.identical
              << ", \"changed\": " << layers.changed
              << ", \"a_only\": " << layers.a_only
              << ", \"b_only\": " << layers.b_only << "}";
    std::cout << ", \"segments\": {\"both\": " << seg_both
              << ", \"a_only\": " << seg_a_only
              << ", \"b_only\": " << seg_b_only
              << ", \"jitter\": " << seg_jitter
              << ", \"percent\": " << seg_percent
              << ", \"layers_checked\": " << seg_layers_checked
              << ", \"layers_dirty\": " << seg_layers_dirty << "}";
    std::cout << ", \"est_seconds\": [" << a.est_seconds << ", " << b.est_seconds << "]";
    std::cout << ", \"layer_count\": [" << a.layer_count << ", " << b.layer_count << "]";

    if (! quiet) {
        std::cout << ", \"changed_config\": [";
        for (size_t i = 0; i < config_rows.size() && i < 40; ++ i)
            std::cout << (i ? ", " : "") << "{\"key\": \"" << json_escape(config_rows[i].key)
                      << "\", \"a\": \"" << json_escape(config_rows[i].a)
                      << "\", \"b\": \"" << json_escape(config_rows[i].b) << "\"}";
        std::cout << "]";

        std::cout << ", \"changed_layers\": [";
        int printed = 0;
        for (const LayerMatch &m : layers.rows) {
            if (! m.changed || printed >= 20)
                continue;
            std::cout << (printed ? ", " : "") << "{\"zkey\": " << m.zkey_a
                      << ", \"d_seconds\": " << m.d_seconds
                      << ", \"d_extrusion\": " << m.d_extrusion
                      << ", \"overlap\": " << m.overlap << ", \"flags\": [";
            for (size_t f = 0; f < m.flags.size(); ++ f)
                std::cout << (f ? ", " : "") << "\"" << json_escape(m.flags[f]) << "\"";
            std::cout << "]}";
            ++ printed;
        }
        std::cout << "]";

        std::cout << ", \"dirty_segment_layers\": [";
        for (size_t i = 0; i < dirty_layers.size(); ++ i)
            std::cout << (i ? ", " : "") << dirty_layers[i];
        std::cout << "]";
    }
    std::cout << "}" << std::endl;
    return ok ? 0 : 1;
}
