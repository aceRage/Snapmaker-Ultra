#include "MeshRound.hpp"

#include "MeshRemesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
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
        // The margin has to clear the FILLET, which is a radius tall, not just a
        // couple of voxels the way a plain remesh needs. Below that the cut plane
        // runs through the rounding and the union leaves a step.
        double margin = opts.bottom_margin;
        if (margin <= 0.)
            margin = ROUND_BOTTOM_MARGIN_RADII * radius;
        margin = std::clamp(margin, 0., ROUND_BOTTOM_MARGIN_MAX);

        std::string why;
        // The voxel_size argument here is what remesh_keep_flat_bottom uses for its
        // base-detection epsilon and for how far it extends the slab past the cut so
        // the two solids overlap. The round ERODES the upper part by up to a radius
        // at its own cut face, so the overlap has to be a radius, not a voxel - pass
        // the radius in that slot and the slab extends far enough to bite.
        out = remesh_keep_flat_bottom(mesh, radius, margin, as_remesher, bed_dir, &why);
        if (out.indices.empty()) {
            BOOST_LOG_TRIVIAL(info) << "MeshRound: keep-bottom-flat not applied (" << why
                                    << "); rounding the whole mesh instead";
            rep.fell_back = true;
            rep.note      = why;
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
