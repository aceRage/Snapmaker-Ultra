#pragma once
// Ultra assembly geometry helpers shared by the Assemble gizmo (guided "Fit for Print & Merge") and
// the Plater auto-fit. Header-only so no build-system change is needed.
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <algorithm>
#include <cmath>
#include <limits>
#include <Eigen/Geometry>
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"

namespace Slic3r { namespace GUI { namespace UltraFit {

// Boundary vertices of a planar patch (edges used by exactly one of `tris`), transformed by W.
inline std::vector<Vec3d> patch_boundary(const indexed_triangle_set& its, const std::vector<int>& tris, const Transform3d& W)
{
    std::map<std::pair<int,int>, int> ecount;
    auto ekey = [](int a, int b){ return a < b ? std::make_pair(a, b) : std::make_pair(b, a); };
    for (int t : tris) {
        if (t < 0 || t >= (int) its.indices.size()) continue;
        const auto& tri = its.indices[t];
        ecount[ekey(tri[0], tri[1])]++; ecount[ekey(tri[1], tri[2])]++; ecount[ekey(tri[2], tri[0])]++;
    }
    std::set<int> used; std::vector<Vec3d> out;
    for (const auto& kv : ecount) {
        if (kv.second != 1) continue;
        for (int vi : { kv.first.first, kv.first.second })
            if (used.insert(vi).second) out.push_back(W * its.vertices[vi].cast<double>());
    }
    return out;
}

struct RollResult {
    bool   ok = false;
    double roll = 0.0;          // radians about the mating normal, through `origin`
    double protrusion = -1.0;   // sum of squared mm the smaller outline pokes outside the larger (0 = fits)
    double clearance = 0.0;     // smallest inside margin of the smaller outline, mm (negative = pokes out)
    bool   attach_is_smaller = true;
};

// A face-pair mate leaves spin about the shared normal free. Choose it so the SMALLER outline protrudes
// LEAST outside the LARGER one ("parts touch, never intersect"); ties -> MAX wall clearance (the centred,
// side-aligned pose) -> least rotation. Both outlines must already be coplanar in the mating plane, i.e.
// the attachment outline is given AFTER the normal-alignment mate. Caller applies
// R = AngleAxisd(result.roll, n) * R.
inline RollResult roll_by_containment(const Vec3d& n_in, const Vec3d& origin,
                                      const std::vector<Vec3d>& target_outline,
                                      const std::vector<Vec3d>& attach_outline)
{
    RollResult r;
    if (target_outline.size() < 3 || attach_outline.size() < 3) return r;
    const Vec3d n  = n_in.normalized();
    const Vec3d bu = n.unitOrthogonal();
    const Vec3d bw = n.cross(bu).normalized(); // (bu,bw,n) right-handed: 2D CCW == AngleAxis(+t, n)
    auto to2 = [&](const Vec3d& P) { const Vec3d d = P - origin; return Eigen::Vector2d(d.dot(bu), d.dot(bw)); };
    std::vector<Eigen::Vector2d> T2, A2;
    T2.reserve(target_outline.size()); A2.reserve(attach_outline.size());
    for (const auto& P : target_outline) T2.push_back(to2(P));
    for (const auto& P : attach_outline) A2.push_back(to2(P));

    // 2D convex hull (Andrew monotone chain), CCW.
    auto hull = [](std::vector<Eigen::Vector2d> q) {
        std::sort(q.begin(), q.end(), [](const Eigen::Vector2d& a, const Eigen::Vector2d& b) {
            return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
        auto cr = [](const Eigen::Vector2d& o, const Eigen::Vector2d& a, const Eigen::Vector2d& b) {
            return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x()); };
        std::vector<Eigen::Vector2d> h(2 * q.size()); size_t k = 0;
        for (size_t i = 0; i < q.size(); ++i) { while (k >= 2 && cr(h[k-2], h[k-1], q[i]) <= 0) --k; h[k++] = q[i]; }
        for (size_t i = q.size() - 1, lo = k + 1; i > 0; --i) { while (k >= lo && cr(h[k-2], h[k-1], q[i-1]) <= 0) --k; h[k++] = q[i-1]; }
        h.resize(k > 1 ? k - 1 : k);
        return h;
    };
    auto area = [](const std::vector<Eigen::Vector2d>& h) {
        double s = 0.0;
        for (size_t i = 0, m = h.size(); i < m; ++i) { const auto& a = h[i]; const auto& b = h[(i + 1) % m]; s += a.x() * b.y() - b.x() * a.y(); }
        return std::abs(s) * 0.5;
    };
    // Signed distance of a point to a CCW convex hull: >0 outside (protrusion), <0 inside (clearance).
    auto sdist = [](const Eigen::Vector2d& p, const std::vector<Eigen::Vector2d>& h) {
        double worst = -std::numeric_limits<double>::infinity();
        for (size_t i = 0, m = h.size(); i < m; ++i) {
            const Eigen::Vector2d a = h[i], e = h[(i + 1) % m] - a; const double L = e.norm(); if (L < 1e-12) continue;
            worst = std::max(worst, -(e.x() * (p.y() - a.y()) - e.y() * (p.x() - a.x())) / L); // >0 = right of edge = outside
        }
        return worst;
    };
    const auto hT = hull(T2), hA = hull(A2);
    if (hT.size() < 3 || hA.size() < 3) return r;
    const bool att_is_small = area(hA) <= area(hT);
    auto rot2 = [](const Eigen::Vector2d& q, double t) { const double c = std::cos(t), s = std::sin(t); return Eigen::Vector2d(c * q.x() - s * q.y(), s * q.x() + c * q.y()); };
    // For a spin t: protrusion = sum of squared outside distances of the smaller outline beyond the larger
    // hull; clearance = the smallest inside margin (negative if anything pokes out).
    auto eval = [&](double t, double& prot, double& clear) {
        prot = 0.0; clear = std::numeric_limits<double>::infinity();
        auto acc = [&](double sd) { if (sd > 0) prot += sd * sd; clear = std::min(clear, -sd); };
        if (att_is_small) {
            for (const auto& q : A2) acc(sdist(rot2(q, t), hT));
        } else {
            std::vector<Eigen::Vector2d> hAr; hAr.reserve(hA.size());
            for (const auto& q : hA) hAr.push_back(rot2(q, t));
            for (const auto& q : T2) acc(sdist(q, hAr));
        }
    };
    double best = std::numeric_limits<double>::infinity(), best_t = 0.0, best_clear = -std::numeric_limits<double>::infinity();
    auto consider = [&](double t) {
        double m, c; eval(t, m, c);
        const bool better = m < best - 1e-3 ||
            (std::abs(m - best) <= 1e-3 && (c > best_clear + 1e-6 ||
                (std::abs(c - best_clear) <= 1e-6 && std::abs(t) < std::abs(best_t))));
        if (better) { best = m; best_t = t; best_clear = c; }
    };
    for (int deg = -180; deg < 180; ++deg) consider(deg * M_PI / 180.0);
    const double coarse_t = best_t;                                          // refine +/-1 deg at 0.05 deg
    for (int k = -20; k <= 20; ++k) consider(coarse_t + k * (0.05 * M_PI / 180.0));
    r.ok = true; r.roll = best_t; r.protrusion = best; r.clearance = best_clear; r.attach_is_smaller = att_is_small;
    return r;
}

struct CollisionRoll {
    bool   ok = false;
    double roll = 0.0;          // radians about the mating axis, through `origin`
    double penetration = -1.0;  // sum of squared mm the attachment samples sit inside the target solid (0 = none)
    int    samples = 0;
};

// Body-level roll: the outline containment only sees the two picked faces, and a curved patch carries no
// rotational cue at all. So spin the ATTACHMENT about the mating axis and keep the angle where its mesh
// penetrates the TARGET's solid least (ties -> least extra spin). `target_w` / `attach_w0` map each mesh's
// own space into the common (print) frame, attach_w0 being the pose AFTER the normal mate.
inline CollisionRoll roll_by_collision(const indexed_triangle_set& tits, const Transform3d& target_w,
                                       const indexed_triangle_set& aits, const Transform3d& attach_w0,
                                       const Vec3d& n_in, const Vec3d& origin)
{
    CollisionRoll r;
    if (tits.indices.empty() || aits.vertices.empty()) return r;
    const Vec3d n = n_in.normalized();
    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tits.vertices, tits.indices);
    if (tree.empty()) return r;
    const Transform3d t_inv = target_w.inverse();
    // Attachment samples in the common frame (pre-spin); skip points on the mating plane (ambiguous sign).
    std::vector<Vec3d> samples; samples.reserve(1600);
    const int stride = std::max(1, int(aits.vertices.size()) / 1500);
    for (size_t i = 0; i < aits.vertices.size(); i += size_t(stride)) {
        const Vec3d P = attach_w0 * aits.vertices[i].cast<double>();
        if (std::abs((P - origin).dot(n)) < 0.15) continue;
        samples.push_back(P);
    }
    if (samples.size() < 8) return r;
    r.samples = (int) samples.size();
    // Penetration of one point into the target solid: its distance to the surface when the closest
    // triangle's outward normal faces away from it (we are behind the surface). Cavities count as outside.
    auto penetration = [&](const Vec3d& Pp) -> double {
        const Vec3d Pm = t_inv * Pp; // into target mesh space
        size_t hit_idx = 0; Vec3d hit_pt = Vec3d::Zero();
        const double d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(tits.vertices, tits.indices, tree, Pm, hit_idx, hit_pt);
        if (d2 < 0.0 || hit_idx >= tits.indices.size()) return 0.0;
        const auto& f = tits.indices[hit_idx];
        const Vec3d a = tits.vertices[f[0]].cast<double>(), b = tits.vertices[f[1]].cast<double>(), c = tits.vertices[f[2]].cast<double>();
        const bool inside = (Pm - hit_pt).dot((b - a).cross(c - a)) < 0.0;
        const double d = std::sqrt(std::max(0.0, d2));
        return (inside && d > 0.05) ? d : 0.0;
    };
    auto eval = [&](double t) {
        const Transform3d spin = Eigen::Translation3d(origin) * Eigen::AngleAxisd(t, n) * Eigen::Translation3d(-origin);
        double m = 0.0;
        for (const auto& P : samples) { const double pen = penetration(spin * P); m += pen * pen; }
        return m;
    };
    double best = std::numeric_limits<double>::infinity(), best_t = 0.0;
    auto consider = [&](double t) {
        const double m = eval(t);
        if (m < best - 1e-6 || (std::abs(m - best) <= 1e-6 && std::abs(t) < std::abs(best_t))) { best = m; best_t = t; }
    };
    for (int deg = -180; deg < 180; deg += 3) consider(deg * M_PI / 180.0);   // coarse
    const double coarse = best_t;
    for (int k = -6; k <= 6; ++k) consider(coarse + k * (0.5 * M_PI / 180.0)); // refine +/-3 deg at 0.5 deg
    r.ok = true; r.roll = best_t; r.penetration = best;
    return r;
}

}}} // namespace Slic3r::GUI::UltraFit
