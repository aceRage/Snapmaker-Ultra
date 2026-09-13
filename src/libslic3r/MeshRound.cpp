#include "MeshRound.hpp"

#include "MeshRemesh.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace Slic3r {

double round_auto_voxel_size(double radius)
{
    if (!(radius > 0.))
        return ROUND_VOXEL_MIN;
    return std::clamp(radius / ROUND_VOXELS_PER_RADIUS, ROUND_VOXEL_MIN, ROUND_VOXEL_MAX);
}

// ---------------------------------------------------------------------------
// Measurement helpers
// ---------------------------------------------------------------------------

double rounded_box_volume(const Vec3d &size, double r)
{
    const double a = size.x(), b = size.y(), c = size.z();
    if (!(r > 0.))
        return a * b * c;
    // A rolling ball of radius r cannot round a box thinner than 2r in any axis;
    // past that the decomposition below goes negative and means nothing.
    if (a <= 2. * r || b <= 2. * r || c <= 2. * r)
        return 0.;

    const double ax = a - 2. * r, bx = b - 2. * r, cx = c - 2. * r;
    // Minkowski sum of the core box (ax, bx, cx) with a ball of radius r:
    //   core + 6 face slabs + 12 quarter-cylinders + 8 corner octants (= 1 sphere).
    const double core  = ax * bx * cx;
    const double slabs = 2. * r * (ax * bx + bx * cx + cx * ax);
    // Four edges run parallel to each axis, each a quarter-cylinder of radius r
    // and length ax / bx / cx: 4 * (pi r^2 / 4) * (ax + bx + cx).
    const double edges   = M_PI * r * r * (ax + bx + cx);
    // Eight corner octants make exactly one sphere.
    const double corners = 4. / 3. * M_PI * r * r * r;
    return core + slabs + edges + corners;
}

double rounded_box_volume_loss(const Vec3d &size, double r)
{
    const double full = size.x() * size.y() * size.z();
    const double v    = rounded_box_volume(size, r);
    if (v <= 0.)
        return 0.;
    return full - v;
}

// A canonical (min, max) vertex-index key for an edge, so the two facets that
// share it agree on the name.
static inline std::pair<int, int> edge_key(int a, int b)
{
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

// Face normals, unnormalised length = 2 * area, so a degenerate facet is a zero
// vector and is skipped rather than producing a NaN direction.
static std::vector<Vec3d> face_normals_d(const indexed_triangle_set &mesh)
{
    std::vector<Vec3d> n(mesh.indices.size(), Vec3d::Zero());
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const Vec3i32 &f = mesh.indices[i];
        if (f[0] < 0 || f[1] < 0 || f[2] < 0 || size_t(f[0]) >= mesh.vertices.size() ||
            size_t(f[1]) >= mesh.vertices.size() || size_t(f[2]) >= mesh.vertices.size())
            continue;
        const Vec3d a = mesh.vertices[f[0]].cast<double>();
        const Vec3d b = mesh.vertices[f[1]].cast<double>();
        const Vec3d c = mesh.vertices[f[2]].cast<double>();
        n[i] = (b - a).cross(c - a);
    }
    return n;
}

double max_dihedral_deg(const indexed_triangle_set &mesh)
{
    if (mesh.indices.empty())
        return 0.;
    const std::vector<Vec3d> normals = face_normals_d(mesh);

    // edge -> the (up to two) facets on it. A third occurrence marks the mesh
    // non-manifold; the angle there is meaningless, so it is skipped.
    std::map<std::pair<int, int>, std::pair<int, int>> edges;
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const Vec3i32 &f = mesh.indices[i];
        for (int s = 0; s < 3; ++s) {
            const auto key = edge_key(f[s], f[(s + 1) % 3]);
            auto       it  = edges.find(key);
            if (it == edges.end())
                edges.emplace(key, std::make_pair(int(i), -1));
            else if (it->second.second < 0)
                it->second.second = int(i);
            else
                it->second.second = -2; // non-manifold marker
        }
    }

    double worst = 0.;
    for (const auto &e : edges) {
        const int f0 = e.second.first, f1 = e.second.second;
        if (f1 < 0)
            continue; // boundary or non-manifold
        const Vec3d &n0 = normals[f0], &n1 = normals[f1];
        const double l0 = n0.norm(), l1 = n1.norm();
        if (l0 < 1e-20 || l1 < 1e-20)
            continue; // degenerate facet
        const double cosang = std::clamp(n0.dot(n1) / (l0 * l1), -1., 1.);
        worst = std::max(worst, std::acos(cosang) * 180. / M_PI);
    }
    return worst;
}

bool is_watertight_manifold(const indexed_triangle_set &mesh)
{
    if (mesh.indices.empty() || mesh.vertices.empty())
        return false;

    std::map<std::pair<int, int>, int> counts;
    for (const Vec3i32 &f : mesh.indices) {
        for (int s = 0; s < 3; ++s) {
            if (f[s] < 0 || size_t(f[s]) >= mesh.vertices.size())
                return false;
            if (f[s] == f[(s + 1) % 3])
                return false; // degenerate facet
            ++counts[edge_key(f[s], f[(s + 1) % 3])];
        }
    }
    for (const auto &c : counts)
        if (c.second != 2)
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Measurement helpers for the base
// ---------------------------------------------------------------------------

// Height above the bed, the same sign convention remesh_height() uses.
static inline double round_height(const Vec3d &d, const Vec3f &v) { return -d.dot(v.cast<double>()); }

// The two axes of the bed plane, so a cross-section can be measured in 2D.
static void round_bed_axes(const Vec3d &bed_dir, Vec3d &u, Vec3d &v)
{
    const Vec3d d = bed_dir.normalized();
    u = d.cross(Vec3d(1., 0., 0.));
    if (u.squaredNorm() < 1e-12)
        u = d.cross(Vec3d(0., 1., 0.));
    u.normalize();
    v = d.cross(u).normalized();
}

double round_base_cap_area(const indexed_triangle_set &mesh, const Vec3d &bed_dir, double eps)
{
    if (mesh.indices.empty())
        return 0.;
    const Vec3d d = bed_dir.normalized();
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &vtx : mesh.vertices)
        z_min = std::min(z_min, round_height(d, vtx));

    double area = 0.;
    for (const Vec3i32 &f : mesh.indices) {
        const Vec3f &a = mesh.vertices[f(0)], &b = mesh.vertices[f(1)], &c = mesh.vertices[f(2)];
        if (round_height(d, a) > z_min + eps || round_height(d, b) > z_min + eps ||
            round_height(d, c) > z_min + eps)
            continue;
        // Unnormalised normal is twice the area; its component along the bed
        // direction is twice the facet's area projected onto the bed plane, positive
        // for a facet that faces the bed.
        const Vec3d  nrm  = (b - a).cast<double>().cross((c - a).cast<double>());
        const double proj = nrm.dot(d);
        if (proj > 0.)
            area += 0.5 * proj;
    }
    return area;
}

double round_base_perimeter_dihedral_deg(const indexed_triangle_set &mesh,
                                         const Vec3d                &bed_dir,
                                         double                      eps,
                                         double                     *max_off_axis_deg)
{
    if (max_off_axis_deg != nullptr)
        *max_off_axis_deg = 0.;
    if (mesh.indices.empty())
        return 0.;

    const Vec3d d = bed_dir.normalized();
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &vtx : mesh.vertices)
        z_min = std::min(z_min, round_height(d, vtx));

    const std::vector<Vec3d> normals = face_normals_d(mesh);
    auto on_plane = [&](int vi) { return round_height(d, mesh.vertices[vi]) <= z_min + eps; };

    // Every facet with at least one vertex on the base plane is either the cap, a
    // wall rising off it, or - if the round filleted the bottom edge - something in
    // between. The worst deviation from "cap or wall" is what tells the two apart.
    if (max_off_axis_deg != nullptr) {
        double worst_off = 0.;
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const Vec3i32 &f = mesh.indices[i];
            if (!on_plane(f(0)) && !on_plane(f(1)) && !on_plane(f(2)))
                continue;
            const double len = normals[i].norm();
            if (len < 1e-20)
                continue;
            // |cos| to the bed direction: 1 for the cap, 0 for a wall.
            const double c = std::clamp(std::abs(normals[i].dot(d) / len), 0., 1.);
            const double ang = std::acos(c) * 180. / M_PI;           // 0 = cap, 90 = wall
            worst_off = std::max(worst_off, std::min(ang, 90. - ang)); // distance to the nearer of the two
        }
        *max_off_axis_deg = worst_off;
    }

    // Dihedral across the edges that LIE in the plane - the bottom perimeter.
    std::map<std::pair<int, int>, std::pair<int, int>> edges;
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const Vec3i32 &f = mesh.indices[i];
        for (int k = 0; k < 3; ++k) {
            const int a = f(k), b = f((k + 1) % 3);
            if (!on_plane(a) || !on_plane(b))
                continue;
            const auto key = edge_key(a, b);
            auto       it  = edges.find(key);
            if (it == edges.end())
                edges.emplace(key, std::make_pair(int(i), -1));
            else if (it->second.second < 0)
                it->second.second = int(i);
            else
                it->second.second = -2;
        }
    }

    double worst = 0.;
    for (const auto &e : edges) {
        const int f0 = e.second.first, f1 = e.second.second;
        if (f1 < 0)
            continue;
        const double l0 = normals[f0].norm(), l1 = normals[f1].norm();
        if (l0 < 1e-20 || l1 < 1e-20)
            continue;
        const double cosang = std::clamp(normals[f0].dot(normals[f1]) / (l0 * l1), -1., 1.);
        worst = std::max(worst, std::acos(cosang) * 180. / M_PI);
    }
    return worst;
}

bool round_section_at_height(const indexed_triangle_set &mesh,
                             const Vec3d                &bed_dir,
                             double                      height,
                             double                     *area,
                             double                     *bbox_size_u,
                             double                     *bbox_size_v)
{
    if (area != nullptr)
        *area = 0.;
    if (bbox_size_u != nullptr)
        *bbox_size_u = 0.;
    if (bbox_size_v != nullptr)
        *bbox_size_v = 0.;
    if (mesh.indices.empty())
        return false;

    const Vec3d d = bed_dir.normalized();
    Vec3d ua, va;
    round_bed_axes(d, ua, va);

    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &vtx : mesh.vertices)
        z_min = std::min(z_min, round_height(d, vtx));
    const double plane = z_min + height;

    // Every facet crossing the plane contributes one segment. The section's area
    // comes straight out of the divergence theorem on those segments, so it needs no
    // loop reconstruction - and it is signed, which is what makes it robust to the
    // segments arriving in arbitrary order.
    double        two_a = 0.;
    double        u_lo = 1e30, u_hi = -1e30, v_lo = 1e30, v_hi = -1e30;
    size_t        segments = 0;
    for (const Vec3i32 &f : mesh.indices) {
        const Vec3d p[3] = {mesh.vertices[f(0)].cast<double>(),
                            mesh.vertices[f(1)].cast<double>(),
                            mesh.vertices[f(2)].cast<double>()};
        const double h[3] = {round_height(d, mesh.vertices[f(0)]),
                             round_height(d, mesh.vertices[f(1)]),
                             round_height(d, mesh.vertices[f(2)])};
        // Crossing points, walking the three edges.
        Vec3d hit[3];
        int   n = 0;
        for (int k = 0; k < 3 && n < 3; ++k) {
            const int    k2 = (k + 1) % 3;
            const double h0 = h[k] - plane, h1 = h[k2] - plane;
            if ((h0 < 0. && h1 >= 0.) || (h1 < 0. && h0 >= 0.))
                hit[n++] = p[k] + (p[k2] - p[k]) * (-h0 / (h1 - h0));
        }
        if (n != 2)
            continue;
        // The segment, oriented so the solid is on its left when seen from above:
        // the facet normal's in-plane part points out of the solid, so ordering the
        // two hits by the cross product with it does the job.
        const Vec3d nrm = (p[1] - p[0]).cross(p[2] - p[0]);
        Vec3d       a2 = hit[0], b2 = hit[1];
        if (nrm.dot(d.cross(b2 - a2)) < 0.)
            std::swap(a2, b2);
        const double au = a2.dot(ua), av = a2.dot(va);
        const double bu = b2.dot(ua), bv = b2.dot(va);
        two_a += au * bv - bu * av;
        u_lo = std::min({u_lo, au, bu}); u_hi = std::max({u_hi, au, bu});
        v_lo = std::min({v_lo, av, bv}); v_hi = std::max({v_hi, av, bv});
        ++segments;
    }
    if (segments == 0)
        return false;

    if (area != nullptr)
        *area = std::abs(two_a) * 0.5;
    if (bbox_size_u != nullptr)
        *bbox_size_u = u_hi - u_lo;
    if (bbox_size_v != nullptr)
        *bbox_size_v = v_hi - v_lo;
    return true;
}

// Do two triangles intersect somewhere other than in geometry they share? The test
// is the standard one - each triangle against the other's plane, then a separating
// axis - with facets that share a vertex index skipped, because touching at a shared
// corner or along a shared edge is what a closed mesh is made of.
static bool tris_intersect(const Vec3d &a0, const Vec3d &a1, const Vec3d &a2,
                           const Vec3d &b0, const Vec3d &b1, const Vec3d &b2,
                           double eps)
{
    const Vec3d na = (a1 - a0).cross(a2 - a0);
    const Vec3d nb = (b1 - b0).cross(b2 - b0);
    if (na.squaredNorm() < 1e-24 || nb.squaredNorm() < 1e-24)
        return false; // degenerate facet: not this test's business

    // Separating-axis test over the 11 candidate axes (2 face normals + 9 edge
    // cross-products). A gap on ANY axis means no intersection.
    const Vec3d ae[3] = {a1 - a0, a2 - a1, a0 - a2};
    const Vec3d be[3] = {b1 - b0, b2 - b1, b0 - b2};
    Vec3d axes[11];
    int   n = 0;
    axes[n++] = na;
    axes[n++] = nb;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            axes[n++] = ae[i].cross(be[j]);

    const Vec3d A[3] = {a0, a1, a2}, B[3] = {b0, b1, b2};
    for (int k = 0; k < n; ++k) {
        const Vec3d ax = axes[k];
        const double len = ax.norm();
        if (len < 1e-12)
            continue; // parallel edges: this axis says nothing
        const Vec3d u = ax / len;
        double a_lo = 1e300, a_hi = -1e300, b_lo = 1e300, b_hi = -1e300;
        for (int i = 0; i < 3; ++i) {
            const double p = A[i].dot(u);
            a_lo = std::min(a_lo, p); a_hi = std::max(a_hi, p);
            const double q = B[i].dot(u);
            b_lo = std::min(b_lo, q); b_hi = std::max(b_hi, q);
        }
        // eps makes mere touching count as separated, so two facets that meet along
        // a seam without sharing indices are not reported.
        if (a_hi < b_lo + eps || b_hi < a_lo + eps)
            return false;
    }
    return true;
}

bool has_no_self_intersections(const indexed_triangle_set &mesh)
{
    const size_t nf = mesh.indices.size();
    if (nf == 0)
        return true;

    // Per-facet AABBs, bucketed into a uniform grid so the pair loop is local rather
    // than quadratic. The cell size is the mean facet extent, which for a remeshed
    // mesh is close to the voxel size.
    std::vector<Vec3d> lo(nf), hi(nf);
    Vec3d gl(1e300, 1e300, 1e300), gh(-1e300, -1e300, -1e300);
    double mean_ext = 0.;
    for (size_t i = 0; i < nf; ++i) {
        const Vec3i32 &f = mesh.indices[i];
        Vec3d l(1e300, 1e300, 1e300), h(-1e300, -1e300, -1e300);
        for (int k = 0; k < 3; ++k) {
            const Vec3d p = mesh.vertices[f(k)].cast<double>();
            l = l.cwiseMin(p); h = h.cwiseMax(p);
        }
        lo[i] = l; hi[i] = h;
        gl = gl.cwiseMin(l); gh = gh.cwiseMax(h);
        mean_ext += (h - l).maxCoeff();
    }
    mean_ext /= double(nf);

    const Vec3d  span = gh - gl;
    const double cell = std::max(mean_ext, span.maxCoeff() / 128.);
    if (!(cell > 0.))
        return true;
    const Eigen::Vector3i dims((std::max)(1, int(std::ceil(span.x() / cell))),
                               (std::max)(1, int(std::ceil(span.y() / cell))),
                               (std::max)(1, int(std::ceil(span.z() / cell))));
    auto cell_of = [&](const Vec3d &p) {
        return Eigen::Vector3i(std::clamp(int((p.x() - gl.x()) / cell), 0, dims.x() - 1),
                               std::clamp(int((p.y() - gl.y()) / cell), 0, dims.y() - 1),
                               std::clamp(int((p.z() - gl.z()) / cell), 0, dims.z() - 1));
    };
    auto key_of = [&](const Eigen::Vector3i &c) {
        return (size_t(c.z()) * size_t(dims.y()) + size_t(c.y())) * size_t(dims.x()) + size_t(c.x());
    };

    std::map<size_t, std::vector<int>> buckets;
    for (size_t i = 0; i < nf; ++i) {
        const Eigen::Vector3i c0 = cell_of(lo[i]), c1 = cell_of(hi[i]);
        for (int z = c0.z(); z <= c1.z(); ++z)
            for (int y = c0.y(); y <= c1.y(); ++y)
                for (int x = c0.x(); x <= c1.x(); ++x)
                    buckets[key_of(Eigen::Vector3i(x, y, z))].push_back(int(i));
    }

    // Scale-relative epsilon: touching within this counts as touching, not crossing.
    const double eps = std::max(1e-9, span.maxCoeff() * 1e-7);
    for (const auto &b : buckets) {
        const std::vector<int> &ids = b.second;
        for (size_t p = 0; p + 1 < ids.size(); ++p) {
            for (size_t q = p + 1; q < ids.size(); ++q) {
                const int i = ids[p], j = ids[q];
                if ((lo[i].array() > hi[j].array() + eps).any() ||
                    (lo[j].array() > hi[i].array() + eps).any())
                    continue;
                const Vec3i32 &fi = mesh.indices[i], &fj = mesh.indices[j];
                bool shares = false;
                for (int a = 0; a < 3 && !shares; ++a)
                    for (int c = 0; c < 3; ++c)
                        if (fi(a) == fj(c)) { shares = true; break; }
                if (shares)
                    continue;
                if (tris_intersect(mesh.vertices[fi(0)].cast<double>(),
                                   mesh.vertices[fi(1)].cast<double>(),
                                   mesh.vertices[fi(2)].cast<double>(),
                                   mesh.vertices[fj(0)].cast<double>(),
                                   mesh.vertices[fj(1)].cast<double>(),
                                   mesh.vertices[fj(2)].cast<double>(),
                                   eps))
                    return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frame helpers. MeshRemesh.cpp has its own copies of these two as file statics;
// duplicating them rather than exporting them keeps that module's surface as it is,
// and they are four lines each.
// ---------------------------------------------------------------------------

// The rotation taking the mesh frame into one where the bed is straight down, i.e.
// bed_dir maps to -Z. Identity for the ordinary unrotated volume, so the usual path
// costs nothing and stays bit-exact.
static Transform3d round_bed_to_z(const Vec3d &bed_dir)
{
    const Vec3d d = bed_dir.normalized();
    const Vec3d target(0., 0., -1.);
    if ((d - target).squaredNorm() < 1e-18)
        return Transform3d::Identity();
    return Transform3d(Eigen::Quaterniond::FromTwoVectors(d, target));
}

static void round_transform_its(indexed_triangle_set &its, const Transform3d &tr)
{
    if (tr.matrix().isIdentity(0.))
        return;
    for (Vec3f &v : its.vertices)
        v = (tr * v.cast<double>()).cast<float>();
}

static void round_z_range(const indexed_triangle_set &its, float &z_min, float &z_max)
{
    z_min = std::numeric_limits<float>::max();
    z_max = std::numeric_limits<float>::lowest();
    for (const Vec3f &v : its.vertices) {
        z_min = std::min(z_min, v.z());
        z_max = std::max(z_max, v.z());
    }
}

// ---------------------------------------------------------------------------
// The zero-slab (mirror) path
// ---------------------------------------------------------------------------

indexed_triangle_set round_mirror_flat_bottom(const indexed_triangle_set &mesh,
                                              double                      radius,
                                              double                      voxel_size,
                                              bool                        round_concave,
                                              const VoxelRounder         &rounder,
                                              const Vec3d                &bed_dir,
                                              std::string                *why)
{
    auto decline = [why](const char *reason) {
        if (why != nullptr)
            *why = reason;
        return indexed_triangle_set{};
    };

    if (mesh.indices.empty() || !rounder || radius <= 0. || voxel_size <= 0.)
        return decline("empty mesh, no rounder, or a non-positive radius");

    // Work with the bed straight down, because cut_mesh() only cuts at a world Z.
    const Transform3d    to_z = round_bed_to_z(bed_dir);
    const Transform3d    back = to_z.inverse();
    indexed_triangle_set work = mesh;
    round_transform_its(work, to_z);

    float z_min = 0.f, z_max = 0.f;
    round_z_range(work, z_min, z_max);
    if (!(z_max > z_min))
        return decline("degenerate mesh height");

    // Same gate as the slab path: with no facet actually resting on the bed there is
    // no base face to protect, and mirroring an arbitrary lowest point produces a
    // spike rather than a flat foot. The caller then rounds the whole mesh, which is
    // what "keep the bottom flat" has always degraded to.
    size_t       bottom_facets = 0;
    const double base_eps      = REMESH_BASE_EPS_VOXELS * voxel_size;
    const double base_area     = remesh_bottom_area(work, Vec3d(0., 0., -1.), base_eps, &bottom_facets);
    if (bottom_facets < 2 || base_area <= 0.)
        return decline("no flat bottom");

    // How far below the base plane the reflection has to reach - see
    // ROUND_MIRROR_DEPTH_RADII for why it is two radii and not one.
    const double depth = std::max(ROUND_MIRROR_DEPTH_RADII * radius,
                                  ROUND_MIRROR_DEPTH_VOXELS * voxel_size);
    // The part must be tall enough to have something above the reflected band, or the
    // two halves of the union meet in the middle of the geometry the user cares about.
    if (double(z_max - z_min) <= depth)
        return decline("part is shorter than the mirror depth");

    // The reflection. Only the bottom `depth` of the part is reflected - that is all
    // the base plane's neighbourhood can see - so a tall part does not pay for a grid
    // twice its height. Cut first, THEN mirror: cutting the original is exact, while
    // mirroring the whole part and cutting the mirror would do the same work on twice
    // the triangles.
    indexed_triangle_set band, discard;
    cut_mesh(work, z_min + float(depth), &discard, &band, true);
    if (band.indices.empty())
        return decline("the bottom band is empty");

    indexed_triangle_set reflected = band;
    for (Vec3f &v : reflected.vertices)
        v.z() = 2.f * z_min - v.z();
    // Reflection reverses handedness, so every facet's winding has to flip or the
    // copy is inside-out and OpenVDB's csgUnion subtracts it instead of adding it.
    its_flip_triangles(reflected);

    // One indexed set holding two solids that share the base face. mesh_to_grid()
    // splits it and csgUnions the parts in the level-set domain, which is exactly the
    // union we want and is immune to the coplanar-face degeneracy that makes a mesh
    // boolean on this pair a coin flip.
    indexed_triangle_set doubled = work;
    its_merge(doubled, reflected);

    indexed_triangle_set rounded = rounder(doubled, radius, voxel_size, round_concave);
    if (rounded.indices.empty())
        return decline("voxel round of the mirrored solid failed");

    // Cut at the base plane and keep the upper half, capped. The base face was
    // interior to `doubled`, so nothing here is a fillet: the cap is the part's own
    // footprint, with its XY corners rounded by the vertical edges' fillet running
    // through the plane, and the wall meets it at a sharp 90.
    indexed_triangle_set upper, below;
    cut_mesh(rounded, z_min, &upper, &below, true);
    if (upper.indices.empty())
        return decline("the cut at the base plane left nothing above it");

    // The cut is computed in float, so a cap vertex can land a hair either side of
    // the plane. Snap the near ones on exactly - "no vertex below z_min" then holds
    // by construction - and treat anything further down as the failure it would be.
    const float snap = float(ROUND_BASE_SNAP_VOXELS * voxel_size);
    for (Vec3f &v : upper.vertices) {
        if (v.z() < z_min) {
            if (z_min - v.z() > snap)
                return decline("a vertex survived the cut below the base plane");
            v.z() = z_min;
        }
    }

    round_transform_its(upper, back);
    return upper;
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

indexed_triangle_set round_with_options(const indexed_triangle_set &mesh,
                                        const RoundOptions         &opts,
                                        const VoxelRounder         &rounder,
                                        const Vec3d                &bed_dir,
                                        RoundReport                *report)
{
    RoundReport  local;
    RoundReport &rep = report != nullptr ? *report : local;
    rep              = RoundReport{};
    rep.triangles_before = mesh.indices.size();

    if (mesh.indices.empty() || !rounder)
        return {};

    const double radius = std::clamp(opts.radius, ROUND_RADIUS_MIN, ROUND_RADIUS_MAX);
    double       voxel  = opts.voxel_size;
    if (voxel <= 0.)
        voxel = round_auto_voxel_size(radius);
    // The offset walks the narrow band by radius/voxel voxels, so a voxel size
    // coarser than the radius allows cannot represent the fillet at all. Clamp it
    // down rather than returning a bad mesh - the user asked for a radius, and the
    // radius is the thing they will measure.
    voxel = std::clamp(voxel, ROUND_VOXEL_MIN, std::min(ROUND_VOXEL_MAX, radius / 2.));

    rep.radius_used = radius;
    rep.voxel_used  = voxel;

    // Bind the four-argument rounder down to MeshRemesh's two-argument
    // VoxelRemesher so remesh_keep_flat_bottom() can drive it. The double it hands
    // us is its own voxel size, which is the one we already decided on; ignoring it
    // is correct, not sloppy - we pass the same value in below.
    const VoxelRemesher as_remesher =
        [&rounder, radius, voxel, &opts](const indexed_triangle_set &m, double) {
            return rounder(m, radius, voxel, opts.round_concave);
        };

    indexed_triangle_set out;
    if (opts.keep_bottom_flat) {
        // Two mechanisms, and which one runs is decided here by the slab height. See
        // RoundOptions::bottom_margin: 0 (the default since the owner's 2026-09-12
        // feedback) mirrors, anything positive keeps a slab of that height.
        const double margin = std::clamp(opts.bottom_margin, 0., ROUND_BOTTOM_MARGIN_MAX);
        rep.bottom_margin_used = margin;

        std::string why;
        if (margin <= 0.) {
            out = round_mirror_flat_bottom(mesh, radius, voxel, opts.round_concave, rounder, bed_dir, &why);
            if (!out.indices.empty())
                rep.mirrored_base = true;
        } else {
            // The voxel_size argument here is what remesh_keep_flat_bottom uses for
            // its base-detection epsilon and for how far it extends the slab past the
            // cut so the two solids overlap. The round ERODES the upper part by up to
            // a radius at its own cut face, so the overlap has to be a radius, not a
            // voxel - pass the radius in that slot and the slab extends far enough to
            // bite.
            out = remesh_keep_flat_bottom(mesh, radius, margin, as_remesher, bed_dir, &why);
        }

        if (out.indices.empty()) {
            BOOST_LOG_TRIVIAL(info) << "MeshRound: keep-bottom-flat not applied ("
                                    << (margin <= 0. ? "mirror: " : "slab: ") << why
                                    << "); rounding the whole mesh instead";
            rep.fell_back          = true;
            rep.note               = why;
            rep.bottom_margin_used = 0.;
        } else {
            rep.kept_bottom_flat = true;
        }
    }

    if (out.indices.empty())
        out = rounder(mesh, radius, voxel, opts.round_concave);
    if (out.indices.empty())
        return {};

    rep.triangles_after = out.indices.size();
    return out;
}

} // namespace Slic3r
