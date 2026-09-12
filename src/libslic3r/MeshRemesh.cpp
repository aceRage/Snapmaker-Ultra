#include "MeshRemesh.hpp"

#include "MeshBoolean.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Slic3r {

double remesh_auto_voxel_size(const indexed_triangle_set &mesh)
{
    if (mesh.indices.empty())
        return 0.1;
    // Unchanged from repair_by_remesh()'s own line, so "auto" in the dialog is
    // exactly what the menu item used to do silently.
    BoundingBoxf3 bb;
    for (const Vec3f &v : mesh.vertices)
        bb.merge(v.cast<double>());
    return std::clamp(bb.size().norm() / 300., 0.05, 0.3);
}

double remesh_height(const Vec3d &bed_dir, const Vec3f &v)
{
    const Vec3d d = bed_dir.normalized();
    return -d.dot(v.cast<double>());
}

// The rotation that takes the mesh frame into a frame where the bed is straight
// down, i.e. bed_dir maps to -Z. Identity for the common unrotated case, so the
// usual path costs nothing and stays bit-exact.
static Transform3d bed_to_z_rotation(const Vec3d &bed_dir)
{
    const Vec3d d = bed_dir.normalized();
    const Vec3d target(0., 0., -1.);
    if ((d - target).squaredNorm() < 1e-18)
        return Transform3d::Identity();
    return Transform3d(Eigen::Quaterniond::FromTwoVectors(d, target));
}

static void transform_its(indexed_triangle_set &its, const Transform3d &tr)
{
    if (tr.matrix().isIdentity(0.))
        return;
    for (Vec3f &v : its.vertices)
        v = (tr * v.cast<double>()).cast<float>();
}

double remesh_bottom_area(const indexed_triangle_set &mesh,
                          const Vec3d                &bed_dir,
                          double                      eps,
                          size_t                     *facet_count)
{
    if (facet_count != nullptr)
        *facet_count = 0;
    if (mesh.indices.empty())
        return 0.;

    const Vec3d d = bed_dir.normalized();
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &v : mesh.vertices)
        z_min = std::min(z_min, remesh_height(d, v));

    double   area = 0.;
    size_t   n    = 0;
    for (const Vec3i32 &f : mesh.indices) {
        const Vec3f &a = mesh.vertices[f(0)];
        const Vec3f &b = mesh.vertices[f(1)];
        const Vec3f &c = mesh.vertices[f(2)];
        if (remesh_height(d, a) > z_min + eps || remesh_height(d, b) > z_min + eps ||
            remesh_height(d, c) > z_min + eps)
            continue;
        // Unnormalised normal is twice the area; its component along the bed
        // direction is twice the area of the facet projected onto the bed plane.
        const Vec3d nrm = (b - a).cast<double>().cross((c - a).cast<double>());
        const double proj = nrm.dot(d);
        // Only facets looking at the bed - an up-facing facet sitting at z_min is
        // not part of the footprint.
        if (proj <= 0.)
            continue;
        area += 0.5 * proj;
        ++n;
    }
    if (facet_count != nullptr)
        *facet_count = n;
    return area;
}

// ---------------------------------------------------------------------------
// Sharp edges
// ---------------------------------------------------------------------------

static std::vector<Vec3f> face_normals_of(const indexed_triangle_set &mesh)
{
    std::vector<Vec3f> normals(mesh.indices.size(), Vec3f::UnitZ());
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        const Vec3i32 &f = mesh.indices[t];
        const Vec3f n = (mesh.vertices[f(1)] - mesh.vertices[f(0)]).cross(mesh.vertices[f(2)] - mesh.vertices[f(0)]);
        const float len = n.norm();
        if (len > 1e-12f)
            normals[t] = n / len;
    }
    return normals;
}

std::vector<SharpEdge> detect_sharp_edges(const indexed_triangle_set &mesh, double dihedral_deg)
{
    std::vector<SharpEdge> out;
    if (mesh.indices.empty())
        return out;

    const std::vector<Vec3f> normals = face_normals_of(mesh);
    const float cos_thresh = float(std::cos(dihedral_deg * M_PI / 180.));

    // Edge key -> (first face, second face). An undirected edge is (min, max) of the
    // two endpoints; the map is over vertex INDICES, so a mesh whose duplicated
    // vertices have not been merged reports its seams as boundary edges - which is
    // the conservative answer (a seam we protect but need not is harmless).
    struct EdgeRec { int f0 = -1; int f1 = -1; };
    std::unordered_map<uint64_t, EdgeRec> edges;
    edges.reserve(mesh.indices.size() * 3);

    auto key = [](int a, int b) -> uint64_t {
        const uint32_t lo = uint32_t(std::min(a, b)), hi = uint32_t(std::max(a, b));
        return (uint64_t(hi) << 32) | uint64_t(lo);
    };

    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        const Vec3i32 &f = mesh.indices[t];
        for (int e = 0; e < 3; ++e) {
            EdgeRec &r = edges[key(f(e), f((e + 1) % 3))];
            if (r.f0 < 0)
                r.f0 = int(t);
            else if (r.f1 < 0)
                r.f1 = int(t);
            // A non-manifold edge (three or more faces) is a feature line by any
            // definition; the first two faces already decided it, so leave it.
        }
    }

    for (const auto &kv : edges) {
        const uint32_t lo = uint32_t(kv.first & 0xffffffffu);
        const uint32_t hi = uint32_t(kv.first >> 32);
        const EdgeRec &r  = kv.second;
        bool sharp = false;
        if (r.f1 < 0)
            sharp = true; // boundary / open edge: a hole rim is a feature too
        else
            sharp = normals[r.f0].dot(normals[r.f1]) < cos_thresh;
        if (!sharp)
            continue;
        SharpEdge se;
        se.a = mesh.vertices[lo];
        se.b = mesh.vertices[hi];
        if ((se.b - se.a).squaredNorm() < 1e-18f)
            continue;
        out.emplace_back(se);
    }
    return out;
}

std::vector<bool> sharp_vertex_mask(const indexed_triangle_set &mesh, double dihedral_deg)
{
    std::vector<bool> hit(mesh.vertices.size(), false);
    if (mesh.indices.empty())
        return hit;

    // Same pairwise-incident-face test SculptSession::detect_sharp_edges() runs,
    // built here from a local incidence list instead of the session's CSR adjacency
    // so the two callers do not have to share a session object.
    const std::vector<Vec3f> normals = face_normals_of(mesh);
    const float cos_thresh = float(std::cos(dihedral_deg * M_PI / 180.));

    std::vector<std::vector<int>> incident(mesh.vertices.size());
    for (size_t t = 0; t < mesh.indices.size(); ++t)
        for (int e = 0; e < 3; ++e)
            incident[size_t(mesh.indices[t](e))].emplace_back(int(t));

    for (size_t v = 0; v < incident.size(); ++v) {
        const std::vector<int> &fs = incident[v];
        for (size_t i = 0; i < fs.size() && !hit[v]; ++i)
            for (size_t j = i + 1; j < fs.size(); ++j)
                if (normals[fs[i]].dot(normals[fs[j]]) < cos_thresh) {
                    hit[v] = true;
                    break;
                }
    }
    return hit;
}

size_t snap_to_sharp_edges(indexed_triangle_set         &its,
                           const std::vector<SharpEdge> &edges,
                           double                        radius)
{
    if (its.vertices.empty() || edges.empty() || radius <= 0.)
        return 0;

    const float r2 = float(radius * radius);
    size_t moved = 0;

    // A uniform grid over the edges keyed by cell, so a big mesh does not pay
    // O(V * E). Cell size = the snap radius, so a vertex only ever has to look at
    // its own cell and the 26 around it.
    const float cell = float(radius);
    auto cell_of = [cell](const Vec3f &p) {
        return Vec3i32(int(std::floor(p.x() / cell)), int(std::floor(p.y() / cell)), int(std::floor(p.z() / cell)));
    };
    auto cell_key = [](const Vec3i32 &c) -> uint64_t {
        // 21 bits per axis, biased; enough for any printable model at these cell sizes.
        const uint64_t x = uint64_t(uint32_t(c.x() + (1 << 20)) & 0x1fffffu);
        const uint64_t y = uint64_t(uint32_t(c.y() + (1 << 20)) & 0x1fffffu);
        const uint64_t z = uint64_t(uint32_t(c.z() + (1 << 20)) & 0x1fffffu);
        return (x << 42) | (y << 21) | z;
    };

    std::unordered_map<uint64_t, std::vector<int>> grid;
    grid.reserve(edges.size() * 2);
    for (size_t e = 0; e < edges.size(); ++e) {
        // Stamp every cell the edge's (radius-inflated) bounding box touches. Edges
        // are short relative to a model, so this stays small.
        const Vec3f lo = edges[e].a.cwiseMin(edges[e].b) - Vec3f::Constant(cell);
        const Vec3f hi = edges[e].a.cwiseMax(edges[e].b) + Vec3f::Constant(cell);
        const Vec3i32 c0 = cell_of(lo), c1 = cell_of(hi);
        // A pathologically long edge would stamp a huge box; cap the work and fall
        // back to brute force for it by stamping nothing and testing it against
        // every vertex below.
        const int64_t cells = int64_t(c1.x() - c0.x() + 1) * int64_t(c1.y() - c0.y() + 1) * int64_t(c1.z() - c0.z() + 1);
        if (cells > 4096)
            continue;
        for (int x = c0.x(); x <= c1.x(); ++x)
            for (int y = c0.y(); y <= c1.y(); ++y)
                for (int z = c0.z(); z <= c1.z(); ++z)
                    grid[cell_key(Vec3i32(x, y, z))].emplace_back(int(e));
    }

    auto closest_on = [](const SharpEdge &e, const Vec3f &p) {
        const Vec3f ab = e.b - e.a;
        const float d2 = ab.squaredNorm();
        if (d2 < 1e-18f)
            return e.a;
        float t = (p - e.a).dot(ab) / d2;
        t = std::clamp(t, 0.f, 1.f);
        return Vec3f(e.a + t * ab);
    };

    for (Vec3f &v : its.vertices) {
        const Vec3i32 c = cell_of(v);
        auto it = grid.find(cell_key(c));
        if (it == grid.end())
            continue;
        float best_d2 = r2;
        Vec3f best    = v;
        for (int ei : it->second) {
            const Vec3f q = closest_on(edges[size_t(ei)], v);
            const float d2 = (q - v).squaredNorm();
            if (d2 < best_d2) {
                best_d2 = d2;
                best    = q;
            }
        }
        if (best_d2 < r2 && (best - v).squaredNorm() > 1e-16f) {
            v = best;
            ++moved;
        }
    }
    return moved;
}

// ---------------------------------------------------------------------------
// Keep the bottom flat - option (ii)
// ---------------------------------------------------------------------------

// The union, Manifold first and mcut as the fallback, matching what the Boolean
// gizmo does.
static bool union_meshes(const indexed_triangle_set &a,
                         const indexed_triangle_set &b,
                         indexed_triangle_set       &out)
{
    const TriangleMesh ma(a), mb(b);
    std::vector<TriangleMesh> dst;
    if (MeshBoolean::mfd::make_boolean(ma, mb, dst, "UNION") && !dst.empty() && !dst.front().its.indices.empty()) {
        out = dst.front().its;
        return true;
    }
    dst.clear();
    try {
        MeshBoolean::mcut::make_boolean(ma, mb, dst, "UNION");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "MeshRemesh: mcut union threw: " << e.what();
        return false;
    }
    if (dst.empty() || dst.front().its.indices.empty())
        return false;
    out = dst.front().its;
    return true;
}

indexed_triangle_set remesh_keep_flat_bottom(const indexed_triangle_set &mesh,
                                             double                      voxel_size,
                                             double                      margin,
                                             const VoxelRemesher        &remesher,
                                             const Vec3d                &bed_dir,
                                             std::string                *why)
{
    auto decline = [why](const char *reason) {
        if (why != nullptr)
            *why = reason;
        return indexed_triangle_set{};
    };

    if (mesh.indices.empty() || voxel_size <= 0. || !remesher)
        return decline("empty mesh or no remesher");
    if (margin <= 0.)
        margin = REMESH_BOTTOM_MARGIN_VOXELS * voxel_size;

    // Work in a frame where the bed is straight down, because cut_mesh() only cuts
    // at a world Z. For the ordinary unrotated volume this rotation is the identity
    // and the mesh is not touched at all, so the slab stays bit-identical.
    const Transform3d to_z   = bed_to_z_rotation(bed_dir);
    const Transform3d back   = to_z.inverse();
    indexed_triangle_set work = mesh;
    transform_its(work, to_z);

    float z_min = std::numeric_limits<float>::max(), z_max = std::numeric_limits<float>::lowest();
    for (const Vec3f &v : work.vertices) {
        z_min = std::min(z_min, v.z());
        z_max = std::max(z_max, v.z());
    }
    if (!(z_max > z_min))
        return decline("degenerate mesh height");

    // No flat bottom: nothing to protect, and cutting would only add a seam. The
    // caller remeshes the whole mesh, which is exactly today's behaviour.
    size_t bottom_facets = 0;
    const double base_eps  = REMESH_BASE_EPS_VOXELS * voxel_size;
    const double base_area = remesh_bottom_area(work, Vec3d(0., 0., -1.), base_eps, &bottom_facets);
    if (bottom_facets == 0 || base_area <= 0.)
        return decline("no flat bottom");
    // A bottom of one or two triangles is a point or a sliver rather than a base;
    // the slab would be degenerate and the boolean would most likely fail. Two is
    // the minimum a legitimate rectangular foot needs, so the cut-off is below it.
    if (bottom_facets < 2)
        return decline("bottom is too few triangles");
    // The slab has to fit under the model with room to spare, or the "upper part"
    // is a sliver and the union has nothing meaningful to do.
    if (margin >= double(z_max - z_min) * 0.5)
        return decline("bottom margin does not leave an upper part");

    const float cut_z = z_min + float(margin);

    indexed_triangle_set upper, lower;
    cut_mesh(work, cut_z, &upper, &lower, true);
    if (upper.indices.empty() || lower.indices.empty())
        return decline("cut produced an empty part");

    // Remesh only the upper part. It keeps its own flat cut cap, which the voxel
    // pass will ripple - but that cap is interior to the union, so the ripple is
    // consumed by the boolean rather than showing on the base.
    indexed_triangle_set upper_remeshed = remesher(upper, voxel_size);
    if (upper_remeshed.indices.empty())
        return decline("voxel remesh of the upper part failed");

    // The remesh shrinks/grows the upper part by up to a voxel, so its cut face no
    // longer reaches down to cut_z and a plain union would leave a gap. Extend the
    // slab up past the cut plane by a voxel so the two solids definitely overlap;
    // the overlap is inside the model and the boolean removes it.
    indexed_triangle_set slab_ext, discard;
    cut_mesh(work, cut_z + float(voxel_size), &discard, &slab_ext, true);
    const indexed_triangle_set &slab = slab_ext.indices.empty() ? lower : slab_ext;

    indexed_triangle_set joined;
    if (!union_meshes(upper_remeshed, slab, joined))
        return decline("boolean union of the remeshed top and the bottom slab failed");

    transform_its(joined, back);
    return joined;
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

indexed_triangle_set remesh_with_options(const indexed_triangle_set &mesh,
                                         const RemeshOptions        &opts,
                                         const VoxelRemesher        &remesher,
                                         const Vec3d                &bed_dir,
                                         RemeshReport               *report)
{
    RemeshReport local;
    RemeshReport &rep = report != nullptr ? *report : local;
    rep = RemeshReport{};
    rep.triangles_before = mesh.indices.size();

    if (mesh.indices.empty() || !remesher)
        return {};

    double voxel = opts.voxel_size;
    if (voxel <= 0.)
        voxel = remesh_auto_voxel_size(mesh);
    voxel = std::clamp(voxel, REMESH_VOXEL_MIN, REMESH_VOXEL_MAX);

    indexed_triangle_set out;
    if (opts.keep_bottom_flat) {
        std::string why;
        out = remesh_keep_flat_bottom(mesh, voxel, opts.bottom_margin, remesher, bed_dir, &why);
        if (out.indices.empty()) {
            // Every decline is benign - the plain remesh is today's behaviour - but
            // say which one it was, because "my base is still rounded" needs an
            // answer in the log.
            BOOST_LOG_TRIVIAL(info) << "MeshRemesh: keep-bottom-flat not applied (" << why
                                    << "); remeshing the whole mesh instead";
            rep.fell_back = true;
            rep.note      = why;
        } else {
            rep.kept_bottom_flat = true;
        }
    }

    if (out.indices.empty())
        out = remesher(mesh, voxel);
    if (out.indices.empty())
        return {};

    if (opts.preserve_sharp_edges) {
        const std::vector<SharpEdge> edges = detect_sharp_edges(mesh, std::clamp(opts.sharp_feature_angle, REMESH_ANGLE_MIN, REMESH_ANGLE_MAX));
        // One voxel: the remesh cannot have moved a feature further than that, so
        // anything beyond is a different part of the surface and must not be dragged.
        rep.sharp_snapped = snap_to_sharp_edges(out, edges, voxel);
    }

    rep.triangles_after = out.indices.size();
    return out;
}

} // namespace Slic3r
