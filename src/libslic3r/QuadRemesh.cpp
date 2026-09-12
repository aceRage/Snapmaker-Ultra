#include "QuadRemesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {

// ---------------------------------------------------------------------------
// The parts that exist whether or not QuadriFlow is compiled in.
// ---------------------------------------------------------------------------

indexed_triangle_set QuadMesh::triangulate() const
{
    indexed_triangle_set its;
    its.vertices = vertices;
    its.indices.reserve(quads.size() * 2);
    for (const Vec4i32 &q : quads) {
        // Split along the 0-2 diagonal. Both triangles keep the quad's winding, so
        // normals are consistent with the quad mesh, and because the two triangles
        // SHARE that diagonal's two vertices (rather than each getting their own
        // copy) a watertight quad mesh stays watertight. Every interior quad edge is
        // still used by exactly two faces afterwards.
        its.indices.emplace_back(q(0), q(1), q(2));
        its.indices.emplace_back(q(0), q(2), q(3));
    }
    return its;
}

int quad_remesh_default_target(const indexed_triangle_set &mesh)
{
    // Two triangles make a quad, so half the triangle count is the face-count-
    // preserving choice - and it is what the spec asks the dialog to prefill.
    const long long half = static_cast<long long>(mesh.indices.size() / 2);
    return int(std::clamp<long long>(half, QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX));
}

bool quad_remesh_accepts(const indexed_triangle_set &mesh, std::string *why)
{
    auto refuse = [why](const char *reason) {
        if (why != nullptr)
            *why = reason;
        return false;
    };

    if (mesh.indices.empty() || mesh.vertices.empty())
        return refuse("the mesh is empty");

    // QuadriFlow's dedge stage builds a half-edge structure and its hierarchy stage
    // walks it assuming every edge has an opposite. An open edge makes that walk run
    // off the end of the surface: upstream either asserts or emits a mesh with holes
    // in unpredictable places. Refusing here - with a reason the user can act on - is
    // much better than handing back quietly wrong geometry. "Repair by remeshing"
    // (MeshRemesh.hpp) is the tool that FIXES this, so the message points at it.
    if (const size_t open = its_num_open_edges(mesh); open > 0) {
        if (why != nullptr)
            *why = "the mesh is not closed (" + std::to_string(open) +
                   " open edge(s)); repair it first with Repair by remeshing";
        return false;
    }

    // Several disconnected shells are individually manifold but QuadriFlow treats the
    // input as one surface: it normalises and scales for a single connected component
    // and its singularity placement is global. Two shells come back fused or
    // mangled, so this is a refusal too.
    if (its_is_splittable(mesh))
        return refuse("the mesh has more than one separate shell; split it into parts first");

    return true;
}

} // namespace Slic3r

#ifndef SLIC3R_QUAD_REMESH

// ---------------------------------------------------------------------------
// Built without the dependency: the entry points exist and refuse, so GUI code can
// call them unconditionally and still compile.
// ---------------------------------------------------------------------------

namespace Slic3r {

bool quad_remesh_available() { return false; }

QuadMesh quad_remesh(const indexed_triangle_set &mesh,
                     const QuadRemeshOptions & /*opts*/,
                     QuadRemeshReport *report)
{
    if (report != nullptr) {
        *report = QuadRemeshReport{};
        report->status           = QuadRemeshStatus::Unavailable;
        report->triangles_before = mesh.indices.size();
        report->note             = "this build does not include the quad remesher";
    }
    return {};
}

} // namespace Slic3r

#else // SLIC3R_QUAD_REMESH

// QuadriFlow's headers are not warning-clean and drag in Boost.Graph and Lemon; keep
// them out of everything else's way by including them only here.
#include <parametrizer.hpp>
#include <optimizer.hpp>

namespace Slic3r {

bool quad_remesh_available() { return true; }

// QuadriFlow is not re-entrant: Hierarchy::Initialize() calls the global srand(), and
// several of its stages use rand() and file-scope scratch state. Two concurrent
// remeshes would interleave those and, worse, make the result depend on the
// interleaving - which is exactly the determinism the slice gates rely on not
// happening. Serialising every call is cheap (a remesh is seconds, and nothing in the
// app runs two at once today) and makes "same seed, same output" true unconditionally.
static std::mutex g_quadriflow_mutex;

QuadMesh quad_remesh(const indexed_triangle_set &mesh,
                     const QuadRemeshOptions    &opts,
                     QuadRemeshReport           *report)
{
    QuadRemeshReport local;
    QuadRemeshReport &rep = report != nullptr ? *report : local;
    rep = QuadRemeshReport{};
    rep.triangles_before = mesh.indices.size();

    auto fail = [&rep](QuadRemeshStatus st, std::string note) {
        rep.status = st;
        rep.note   = std::move(note);
        return QuadMesh{};
    };

    if (mesh.indices.empty() || mesh.vertices.empty())
        return fail(QuadRemeshStatus::EmptyInput, "the mesh is empty");

    std::string why;
    if (!quad_remesh_accepts(mesh, &why))
        return fail(QuadRemeshStatus::NotManifold, why);

    const int target = opts.target_faces > 0
        ? std::clamp(opts.target_faces, QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX)
        : quad_remesh_default_target(mesh);
    if (target <= 0)
        return fail(QuadRemeshStatus::EmptyInput, "the target face count is empty");

    std::lock_guard<std::mutex> lock(g_quadriflow_mutex);

    QuadMesh out;
    try {
        qflow::Parametrizer field;

        // Feed the mesh in memory. Upstream's own entry point is Load(), which parses
        // an .obj from disk - a temp file round-trip we do not need and which would
        // also lose precision through the text format. Load() is literally
        // `load(); NormalizeMesh();`, so filling V and F and calling NormalizeMesh()
        // ourselves is the same thing minus the file. (Both are column-major: column
        // i is vertex/face i.)
        field.V.resize(3, Eigen::Index(mesh.vertices.size()));
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vec3f &v = mesh.vertices[i];
            field.V.col(Eigen::Index(i)) = Eigen::Vector3d(double(v.x()), double(v.y()), double(v.z()));
        }
        field.F.resize(3, Eigen::Index(mesh.indices.size()));
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const Vec3i32 &f = mesh.indices[i];
            field.F.col(Eigen::Index(i)) = Eigen::Vector3i(f(0), f(1), f(2));
        }
        field.NormalizeMesh();

        field.flag_preserve_sharp = opts.preserve_sharp ? 1 : 0;
        // Deliberately left off:
        //   flag_preserve_boundary - we refuse open meshes, so there is no boundary;
        //   flag_adaptive_scale    - the dialog promises an even grid at a target
        //                            count, and adaptive scaling trades that away;
        //   flag_aggresive_sat     - the one path that would shell out to an external
        //                            `minisat` binary (src/localsat.cpp). Never set,
        //                            so we never depend on a tool the user lacks.
        field.flag_preserve_boundary = 0;
        field.flag_adaptive_scale    = 0;
        field.flag_aggresive_sat     = 0;
        field.flag_minimum_cost_flow = 0;
        // The seed. Hierarchy::Initialize() does srand(rng_seed) and
        // parametrizer-int.cpp seeds its own generator from it, so this is the single
        // knob that makes a run reproducible.
        field.hierarchy.rng_seed = int(opts.seed);

        // Upstream's pipeline, in main.cpp's order. Kept explicit rather than wrapped,
        // so a future upstream bump shows up as a diff here instead of silently
        // changing behaviour.
        field.Initialize(target);
        qflow::Optimizer::optimize_orientations(field.hierarchy);
        field.ComputeOrientationSingularities();
        qflow::Optimizer::optimize_scale(field.hierarchy, field.rho, field.flag_adaptive_scale);
        field.flag_adaptive_scale = 1;   // main.cpp sets this between the two stages
        qflow::Optimizer::optimize_positions(field.hierarchy, field.flag_adaptive_scale);
        field.ComputePositionSingularities();
        field.ComputeIndexMap();

        if (field.F_compact.empty() || field.O_compact.empty())
            return fail(QuadRemeshStatus::Failed, "the quad remesher produced no faces");

        // Undo NormalizeMesh()'s unit-cube fit. OutputMesh() does exactly this before
        // writing its .obj; we apply it here instead of going through a file.
        out.vertices.reserve(field.O_compact.size());
        for (const auto &p : field.O_compact) {
            const Eigen::Vector3d w = p * field.normalize_scale + field.normalize_offset;
            out.vertices.emplace_back(float(w.x()), float(w.y()), float(w.z()));
        }
        out.quads.reserve(field.F_compact.size());
        for (const auto &q : field.F_compact)
            out.quads.emplace_back(q[0], q[1], q[2], q[3]);
    } catch (const std::exception &ex) {
        // QuadriFlow asserts and throws on inputs its hierarchy cannot handle. A
        // failed remesh must leave the user's model alone, not take the app down.
        return fail(QuadRemeshStatus::Failed, std::string("the quad remesher failed: ") + ex.what());
    } catch (...) {
        return fail(QuadRemeshStatus::Failed, "the quad remesher failed");
    }

    if (out.quads.empty())
        return fail(QuadRemeshStatus::Failed, "the quad remesher produced no faces");

    rep.status          = QuadRemeshStatus::Ok;
    rep.quads_after     = out.quads.size();
    rep.triangles_after = out.quads.size() * 2;
    return out;
}

} // namespace Slic3r

#endif // SLIC3R_QUAD_REMESH

namespace Slic3r {

indexed_triangle_set quad_remesh_triangulated(const indexed_triangle_set &mesh,
                                              const QuadRemeshOptions    &opts,
                                              QuadRemeshReport           *report)
{
    const QuadMesh qm = quad_remesh(mesh, opts, report);
    if (qm.empty())
        return {};
    return qm.triangulate();
}

} // namespace Slic3r
