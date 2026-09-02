// Split by painted colour, stage 3: the Manifold side - mesh conversion and the sequential-Split partition.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md, 3.1a and 3.8.
#include "ColorSplit.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include <manifold/manifold.h>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

// ---- Manifold conversion and the sequential-Split partition (spec 3.8) -----------------------------------

// MeshGL64 is MeshGLP<double, uint64_t> (mesh.h:101-163): double coordinates, uint64 triangle indices. The
// explicit tolerance is spec 3.7's - Manifold otherwise derives one from the bounding box, and a shell whose
// bottom sits microns below the surface must not be simplified away. AsOriginal() (manifold.h:198-200) stamps
// a fresh OriginalID on the result; that ID is what the island pass below reads back off the Split output to
// tell which input each face came from. The refinement pre-pass has no use for that provenance and passes
// as_original = false: the header says nothing about what AsOriginal does to the triangulation it stamps
// over, and a flat refinement's whole product is coplanar triangles, so it does not ask.
manifold::Manifold to_manifold64(const indexed_triangle_set &its, manifold::ExecutionContext &ctx, bool as_original = true)
{
    manifold::MeshGL64 m;
    m.numProp = 3;
    m.vertProperties.reserve(its.vertices.size() * 3);
    for (const Vec3f &v : its.vertices) {
        m.vertProperties.push_back(v.x());
        m.vertProperties.push_back(v.y());
        m.vertProperties.push_back(v.z());
    }
    m.triVerts.reserve(its.indices.size() * 3);
    for (const Vec3i32 &t : its.indices) {
        m.triVerts.push_back(uint64_t(t[0]));
        m.triVerts.push_back(uint64_t(t[1]));
        m.triVerts.push_back(uint64_t(t[2]));
    }
    // mesh.h:159-162: the tolerance actually used is the MAXIMUM of this and a baseline derived from the
    // bounding box, and any edge shorter than it may be collapsed - so this cannot switch simplification off,
    // it only pins the floor at the spec's value (3.7). 1e-5 mm is well below any print resolution and above
    // float noise on part-sized coordinates.
    m.tolerance = 1e-5;
    m.Merge();            // fuse duplicated vertices so a triangle soup can still form a topological manifold
    // FromMeshGL (common.h:262-271) is the ctx-aware ingest: its heavy phases check for cancellation, which
    // plain Manifold(m) does not. AsOriginal() then stamps the provenance ID and drops the attachment, so
    // callers re-attach with WithContext.
    const manifold::Manifold out = ctx.FromMeshGL(m);
    return as_original ? out.AsOriginal() : out;
}

// Exporting a MeshGL64 is the expensive step, so callers that already hold one (the island pass needs it
// for the provenance runs) convert straight from it rather than asking the Manifold for a second copy.
indexed_triangle_set from_meshgl64(const manifold::MeshGL64 &out)
{
    indexed_triangle_set its;
    const size_t stride = size_t(out.numProp);                                    // mesh.h:110 - always >= 3
    const size_t nv     = stride > 0 ? out.vertProperties.size() / stride : 0;
    its.vertices.reserve(nv);
    for (size_t i = 0; i < nv; ++i)
        its.vertices.emplace_back(float(out.vertProperties[i * stride]), float(out.vertProperties[i * stride + 1]),
                                  float(out.vertProperties[i * stride + 2]));
    its.indices.reserve(out.triVerts.size() / 3);
    for (size_t i = 0; i + 2 < out.triVerts.size(); i += 3)
        its.indices.emplace_back(int(out.triVerts[i]), int(out.triVerts[i + 1]), int(out.triVerts[i + 2]));
    return its;
}

indexed_triangle_set from_manifold(const manifold::Manifold &m) { return from_meshgl64(m.GetMeshGL64()); }

void require_ok(const manifold::Manifold &m, const char *what)
{
    const manifold::Manifold::Error status = m.Status();
    if (status == manifold::Manifold::Error::Cancelled) throw ColorSplitCancelled();
    if (status != manifold::Manifold::Error::NoError)
        throw ColorSplitError(std::string("Boolean failed (") + what + ", Manifold status " + std::to_string(int(status)) + ").");
}

// Faces of one Split output grouped by the OriginalID of the input they came from. mesh.h:125-138: the runs
// cover all of triVerts and runIndex is one longer than runOriginalID - if that ever stops holding, the
// provenance this pass relies on is not what we think it is, so say so instead of guessing.
std::map<uint32_t, size_t> faces_by_original_id(const manifold::MeshGL64 &gl)
{
    if (gl.runIndex.size() != gl.runOriginalID.size() + 1)
        throw ColorSplitError("Manifold returned no usable face provenance (internal error).");
    std::map<uint32_t, size_t> faces;
    for (size_t run = 0; run < gl.runOriginalID.size(); ++run) {
        // Manifold emits an EMPTY run for an input that contributed nothing to this component (measured on
        // the fully painted sphere: the leftover core carries a 0-triangle run for the source mesh). Counting
        // those would make every island look as if it touched the source and no island would ever be absorbed.
        const size_t n = size_t(gl.runIndex[run + 1] - gl.runIndex[run]) / 3;
        if (n > 0) faces[gl.runOriginalID[run]] += n;
    }
    return faces;
}

} // namespace

ColorPatches refine_color_patches(const ColorPatches &patches, double max_edge_mm)
{
    if (!(max_edge_mm > 0.) || !std::isfinite(max_edge_mm))
        return patches;
    // RefineToLength splits an edge into ceil(length / max_edge_mm) pieces, so a surface whose edges are all
    // short already comes back identical. Checking that here keeps an ordinary mesh - a 100k-triangle sphere,
    // say - out of the Manifold round trip altogether, which is what spec 3.1a's "barely touched" promises.
    bool too_long = false;
    for (const Vec3i32 &t : patches.surface.indices)
        for (int k = 0; k < 3 && !too_long; ++k)
            too_long = (patches.surface.vertices[t[(k + 1) % 3]] - patches.surface.vertices[t[k]]).norm() > max_edge_mm;
    if (!too_long)
        return patches;

    manifold::ExecutionContext ctx;
    const manifold::Manifold source = to_manifold64(patches.surface, ctx, /*as_original=*/false);
    require_ok(source, "refinement input");
    // RefineToLength is an EAGER op, so it observes the attached context (manifold.h:148-166). The header
    // files it under "Smoothing" but documents no per-function behaviour, so the refinement being FLAT is
    // pinned by test instead ("refinement adds interior side vertices to a two-ring cylinder"): the refined
    // cylinder keeps the coarse one's volume to 1e-5 and its new side vertices sit on the chords with exactly
    // radial normals, which a curved refinement would not produce.
    const manifold::Manifold refined = source.WithContext(ctx).RefineToLength(max_edge_mm);
    require_ok(refined, "refinement");
    if (refined.NumTri() <= source.NumTri())
        return patches;

    ColorPatches out;
    out.surface = from_manifold(refined);
    out.states  = patches.states;
    if (its_num_open_edges(out.surface) != 0)
        throw ColorSplitError("Surface refinement did not preserve the closed surface (internal error).");
    // Every refined triangle lies inside one triangle of F, so the facet of F nearest its centroid IS its
    // parent (distance zero) and hands over the state the paint gave that parent.
    const AABBMesh parent(patches.surface);
    out.facet_state.resize(out.surface.indices.size());
    for (size_t f = 0; f < out.surface.indices.size(); ++f) {
        const Vec3i32 &t = out.surface.indices[f];
        const Vec3d centroid = ((out.surface.vertices[t[0]] + out.surface.vertices[t[1]] + out.surface.vertices[t[2]]) / 3.f).cast<double>();
        int   face = 0;
        Vec3d closest;
        parent.squared_distance(centroid, face, closest);
        out.facet_state[f] = patches.facet_state[size_t(face)];
    }
    return out;
}

ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells, bool absorb_islands, const ColorSplitProgress &progress)
{
    // The context is the plumbing spec rev 2 (M7) asks for, and require_ok turns Error::Cancelled into
    // ColorSplitCancelled - but note that Split is an EAGER op that does not observe an attached ctx
    // (manifold.h:147-171: only Refine / Hull / Minkowski and a deferred tree's Status() do). Cancellation
    // therefore lands BETWEEN shells, not inside a boolean: each Split runs to completion.
    manifold::ExecutionContext ctx;
    auto tick = [&](int pct) { if (progress && !progress(pct)) { ctx.Cancel(); throw ColorSplitCancelled(); } };

    manifold::Manifold original = to_manifold64(mesh, ctx).WithContext(ctx);
    require_ok(original, "source mesh");
    const uint32_t original_id  = uint32_t(original.OriginalID());
    const double   vol_original = original.Volume();

    ColorSplitResult r;
    // (volume, mesh): every Manifold is exported to a MeshGL64 exactly once, where we first have one in hand.
    using Part = std::pair<double, indexed_triangle_set>;
    std::map<int, std::vector<Part>> piece_parts;   // filament -> its share of the part
    std::map<uint32_t, int>          shell_state;   // shell OriginalID -> filament
    // Spec 3.8/7: a filament can lose ALL of its area to a lower one, in which case piece_parts never gains a
    // key for it. The states have to be remembered up front or that colour would vanish without a word.
    std::set<int> shell_states;
    for (const ColorShell &s : shells) shell_states.insert(s.state);
    manifold::Manifold rest = original;
    for (size_t i = 0; i < shells.size(); ++i) {
        manifold::Manifold shell = to_manifold64(shells[i].mesh, ctx).WithContext(ctx);
        require_ok(shell, "shell");
        shell_state[uint32_t(shell.OriginalID())] = shells[i].state;
        // manifold.cpp:950-967: Split returns (intersection, difference) - the piece first, the remainder
        // second, from one evaluation, so the two are complementary with no epsilon mismatch between them.
        auto [piece, remainder] = rest.Split(shell);
        require_ok(piece, "split piece"); require_ok(remainder, "split remainder");
        if (!piece.IsEmpty()) piece_parts[shells[i].state].emplace_back(piece.Volume(), from_manifold(piece));
        rest = remainder;
        tick(int(50 + 40 * double(i + 1) / double(shells.size())));
    }

    // Spec 3.8: enclosed body islands -> the colour that contributed most of their surface. A component none
    // of whose faces come from the source mesh is completely wrapped by colour pieces and therefore invisible.
    std::vector<Part> body_parts;
    for (manifold::Manifold &comp : rest.Decompose()) {
        require_ok(comp, "body component");
        const manifold::MeshGL64         gl    = comp.GetMeshGL64();   // one export, used for both purposes
        const std::map<uint32_t, size_t> faces = faces_by_original_id(gl);
        const bool touches_original = faces.count(original_id) != 0;
        if (absorb_islands && !touches_original && !faces.empty()) {
            uint32_t best = 0; size_t best_n = 0; int best_state = std::numeric_limits<int>::max();
            for (const std::pair<const uint32_t, size_t> &face_run : faces) {
                auto      it = shell_state.find(face_run.first);
                const int st = it != shell_state.end() ? it->second : std::numeric_limits<int>::max();
                // Most surface wins; ties go to the lowest filament.
                if (face_run.second > best_n || (face_run.second == best_n && st < best_state)) {
                    best = face_run.first; best_n = face_run.second; best_state = st;
                }
            }
            auto winner = shell_state.find(best);
            if (winner != shell_state.end()) { piece_parts[winner->second].emplace_back(comp.Volume(), from_meshgl64(gl)); continue; }
        }
        body_parts.emplace_back(comp.Volume(), from_meshgl64(gl));
    }

    double total = 0.;
    for (Part &b : body_parts) { total += b.first; its_merge(r.body, b.second); }
    for (auto &[state, parts] : piece_parts) {
        indexed_triangle_set its;
        for (Part &part : parts) { total += part.first; its_merge(its, part.second); }
        r.pieces.emplace_back(state, std::move(its));
    }
    // Spec 3.8: empty pieces are dropped with a warning - the colour was painted but a lower filament claimed
    // every last bit of it (or its shell fell outside the part), and the user has to hear about that.
    for (int state : shell_states)
        if (piece_parts.count(state) == 0)
            r.warnings.push_back("Filament " + std::to_string(state) + ": painted area produced no solid (fully covered by lower filaments).");
    if (std::abs(total - vol_original) > 1e-4 * std::abs(vol_original) + 1e-9)
        throw ColorSplitError("Volume check failed after splitting (" + std::to_string(total) + " vs " + std::to_string(vol_original) + " mm^3).");
    tick(95);
    return r;
}

} // namespace Slic3r
