#include "DrawCut.hpp"
#include "CurvedCut.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

const char* draw_cut_error_message(DrawCutError err)
{
    switch (err) {
    case DrawCutError::None:             return "";
    case DrawCutError::TooShort:         return "Draw a longer line on the model";
    case DrawCutError::SelfCrossing:     return "The line crosses itself";
    case DrawCutError::LeavesMesh:       return "The line leaves the model";
    case DrawCutError::CutterDegenerate: return "The line does not make a usable cut surface";
    case DrawCutError::EmptySide:        return "The stroke does not separate the part";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Resample
// ---------------------------------------------------------------------------

static Vec3d safe_normalize(const Vec3d& v, const Vec3d& fallback)
{
    const double n = v.norm();
    return n > 1e-12 ? Vec3d(v / n) : fallback;
}

std::vector<DrawCutSample> draw_cut_resample(const std::vector<DrawCutSample>& in, double spacing, bool closed)
{
    std::vector<DrawCutSample> out;
    if (in.size() < 2 || spacing <= 0.0)
        return in;

    // Drop zero-length spans first: a captured stroke can hold two identical hits
    // (the mouse did not move between two motion events), and a zero span has no
    // direction to lerp along.
    std::vector<DrawCutSample> src;
    src.reserve(in.size() + 1);
    src.push_back(in.front());
    for (size_t i = 1; i < in.size(); ++ i)
        if ((in[i].pos - src.back().pos).norm() > 1e-12)
            src.push_back(in[i]);
    // A ring walks its closing span too, which is expressed by appending the first
    // sample - the output then drops that duplicate, because the strip builder
    // wraps and a duplicate would hand it a zero-length span.
    if (closed && (src.back().pos - src.front().pos).norm() > 1e-12)
        src.push_back(src.front());
    if (src.size() < 2)
        return in;

    // Cumulative arc length at each source sample, so the walk below is a plain
    // "where along the polyline is distance d" lookup rather than per-span
    // bookkeeping with a carry (which is exactly where an off-by-one hides).
    std::vector<double> acc(src.size(), 0.0);
    for (size_t i = 1; i < src.size(); ++ i)
        acc[i] = acc[i - 1] + (src[i].pos - src[i - 1].pos).norm();
    const double total = acc.back();
    if (total < 1e-12)
        return in;

    auto sample_at = [&src, &acc](double d, size_t& hint) {
        while (hint + 2 < src.size() && acc[hint + 1] < d)
            ++ hint;
        const double span = acc[hint + 1] - acc[hint];
        const double t    = span > 1e-12 ? std::clamp((d - acc[hint]) / span, 0.0, 1.0) : 0.0;
        DrawCutSample smp;
        smp.pos    = src[hint].pos + t * (src[hint + 1].pos - src[hint].pos);
        smp.normal = safe_normalize((1.0 - t) * src[hint].normal + t * src[hint + 1].normal, src[hint].normal);
        // The facet comes from whichever end is nearer, which is what a
        // re-derivation would give anyway at these spacings.
        smp.facet  = t < 0.5 ? src[hint].facet : src[hint + 1].facet;
        return smp;
    };

    size_t hint = 0;
    // Every emitted sample sits at an exact multiple of `spacing`, so the interior
    // spans are `spacing` long to floating-point precision - not to within an
    // accumulated drift.
    const size_t steps = size_t(std::floor(total / spacing));
    out.reserve(steps + 2);
    for (size_t k = 0; k <= steps; ++ k)
        out.push_back(sample_at(double(k) * spacing, hint));

    if (closed) {
        // The ring stays OPEN: drop a trailing sample that has come back onto the
        // first one, so the wrap span is a real span. (total is the full
        // circumference here, so out.back() is at most one spacing short of it.)
        while (out.size() > 3 && (out.back().pos - out.front().pos).norm() < 0.5 * spacing)
            out.pop_back();
    }
    else {
        // equally_spaced_points emits the first point but not reliably the last.
        // Fix that here rather than inheriting the wart: append the final input
        // sample, unless it lands within half a spacing of what is already there,
        // where appending it would leave a short span that the central-difference
        // tangent would then read as a near-zero direction.
        if ((out.back().pos - src.back().pos).norm() > 0.5 * spacing)
            out.push_back(src.back());
    }

    return out;
}

void draw_cut_smooth(std::vector<DrawCutSample>& path, int passes, bool closed, double strength)
{
    if (passes <= 0 || path.size() < 3)
        return;
    const double w = std::clamp(strength, 0.0, 1.0);
    if (w <= 0.0)
        return;

    const size_t n = path.size();
    std::vector<DrawCutSample> next(n);
    for (int pass = 0; pass < passes; ++ pass) {
        for (size_t i = 0; i < n; ++ i) {
            // An open stroke's endpoints are HELD: moving them would shorten the
            // stroke a little on every pass, and the ends are exactly where the
            // user decided the cut should reach.
            if (!closed && (i == 0 || i + 1 == n)) {
                next[i] = path[i];
                continue;
            }
            const DrawCutSample& prev = path[(i + n - 1) % n];
            const DrawCutSample& cur  = path[i];
            const DrawCutSample& nxt  = path[(i + 1) % n];
            next[i].pos    = (1.0 - w) * cur.pos + w * (0.5 * (prev.pos + nxt.pos));
            next[i].normal = safe_normalize((1.0 - w) * cur.normal + w * (0.5 * (prev.normal + nxt.normal)), cur.normal);
            next[i].facet  = cur.facet;
        }
        path.swap(next);
    }
}

int draw_cut_smooth_passes(double smoothing)
{
    const double s = std::clamp(smoothing, 0.0, 1.0);
    return int(std::lround(s * double(DrawCutStroke::MaxSmoothPasses)));
}

double draw_cut_closing_tolerance(double spacing)
{
    return std::max(3.0 * std::max(spacing, 0.0), 2.0);
}

// ---------------------------------------------------------------------------
// DrawCutStroke
// ---------------------------------------------------------------------------

void DrawCutStroke::append(const Vec3d& pos, const Vec3d& normal, size_t facet)
{
    DrawCutSample s;
    s.pos    = pos;
    s.normal = safe_normalize(normal, Vec3d::UnitZ());
    s.facet  = facet;
    m_samples.push_back(s);
}

void DrawCutStroke::clear()
{
    m_samples.clear();
    m_path.clear();
    m_binormal.clear();
    m_closed = false;
    m_error  = DrawCutError::TooShort;
}

DrawCutError DrawCutStroke::finish(double spacing, double smoothing, bool force_closed)
{
    m_path.clear();
    m_binormal.clear();
    m_closed = false;
    m_error  = DrawCutError::None;

    if (m_samples.size() < 2) {
        m_error = DrawCutError::TooShort;
        return m_error;
    }

    const double sp  = spacing > 0.0 ? spacing : DefaultSpacing;
    const double tol = draw_cut_closing_tolerance(sp);

    // THE JUMP TEST, before anything else. A miss during capture is skipped, not
    // fatal (crossing a hole or the silhouette must not end the stroke), but that
    // leaves the raw samples with a gap where the ray found nothing - and a gap
    // wider than a few spacings means the stroke jumped across empty space.
    // Bridging it would run a chord through air, so keep the LONGEST contiguous
    // run instead and let the length gate below decide whether what is left is
    // enough. The threshold is generous (8 spacings) because a fast flick through
    // a thin feature legitimately leaves a wide-ish gap that the interpolation
    // loop could not fill.
    const double jump = 8.0 * sp;
    std::vector<DrawCutSample> run;
    bool discarded_a_run = false;
    {
        size_t best_begin = 0, best_end = 0; // [begin, end)
        size_t begin = 0;
        double best_len = -1.0, cur_len = 0.0;
        for (size_t i = 1; i <= m_samples.size(); ++ i) {
            const bool  broke = i == m_samples.size() ||
                                (m_samples[i].pos - m_samples[i - 1].pos).norm() > jump;
            if (!broke) {
                cur_len += (m_samples[i].pos - m_samples[i - 1].pos).norm();
                continue;
            }
            if (cur_len > best_len) {
                best_len   = cur_len;
                best_begin = begin;
                best_end   = i;
            }
            begin   = i;
            cur_len = 0.0;
        }
        run.assign(m_samples.begin() + int(best_begin), m_samples.begin() + int(best_end));
        discarded_a_run = best_end - best_begin < m_samples.size();
        if (discarded_a_run)
            BOOST_LOG_TRIVIAL(warning) << "Draw cut: the stroke jumped across empty space; keeping the longest run ("
                                       << (best_end - best_begin) << " of " << m_samples.size() << " samples)";
    }
    // WHICH ERROR. When the repair kept a run and that run is usable, there is no
    // error - the whole point of keeping the longest run is that a stroke which
    // crossed a hole still works. But when what is left is too short, the REASON
    // the user needs is "your line left the model", not "draw a longer line":
    // their line was long enough, it just was not all on the part. So the
    // discarded-run flag decides which of the two gates reports.
    const DrawCutError short_error = discarded_a_run ? DrawCutError::LeavesMesh : DrawCutError::TooShort;
    if (run.size() < 2) {
        m_error = short_error;
        return m_error;
    }

    // Open or closed, decided on the RAW run - the resampler needs to know, since
    // a ring resamples its closing span too.
    const double gap = (run.back().pos - run.front().pos).norm();
    m_closed = force_closed || gap <= tol;
    // A "closed" stroke of three raw samples is not a loop, it is a wobble.
    if (m_closed && run.size() < 3)
        m_closed = false;

    m_path = draw_cut_resample(run, sp, m_closed);

    // Smoothing. A closed path wraps, an open one holds its ends.
    draw_cut_smooth(m_path, draw_cut_smooth_passes(smoothing), m_closed);

    if (m_path.size() < size_t(MinSamples) || length() < MinLength) {
        m_error = short_error;
        return m_error;
    }

    compute_binormals();

    if (draw_cut_self_crossing(*this)) {
        m_error = DrawCutError::SelfCrossing;
        return m_error;
    }

    return m_error;
}

double DrawCutStroke::length() const
{
    if (m_path.size() < 2)
        return 0.0;
    double len = 0.0;
    for (size_t i = 1; i < m_path.size(); ++ i)
        len += (m_path[i].pos - m_path[i - 1].pos).norm();
    if (m_closed)
        len += (m_path.front().pos - m_path.back().pos).norm();
    return len;
}

Vec3d DrawCutStroke::centroid() const
{
    if (m_path.empty())
        return Vec3d::Zero();
    Vec3d c = Vec3d::Zero();
    for (const DrawCutSample& s : m_path)
        c += s.pos;
    return c / double(m_path.size());
}

Vec3d DrawCutStroke::tangent(size_t i) const
{
    const size_t n = m_path.size();
    if (n < 2)
        return Vec3d::UnitX();
    if (m_closed) {
        const Vec3d d = m_path[(i + 1) % n].pos - m_path[(i + n - 1) % n].pos;
        return safe_normalize(d, Vec3d::UnitX());
    }
    // Open: one-sided at the ends, central in the middle.
    if (i == 0)
        return safe_normalize(m_path[1].pos - m_path[0].pos, Vec3d::UnitX());
    if (i + 1 >= n)
        return safe_normalize(m_path[n - 1].pos - m_path[n - 2].pos, Vec3d::UnitX());
    return safe_normalize(m_path[i + 1].pos - m_path[i - 1].pos, Vec3d::UnitX());
}

void DrawCutStroke::compute_binormals()
{
    const size_t n = m_path.size();
    m_binormal.assign(n, Vec3d::Zero());
    if (n == 0)
        return;

    const Vec3d c = centroid();

    for (size_t i = 0; i < n; ++ i) {
        const Vec3d t = tangent(i);
        const Vec3d nrm = m_path[i].normal;
        Vec3d b = t.cross(nrm);
        if (b.norm() < 1e-9) {
            // t parallel to n: the stroke runs along its own normal, which cannot
            // happen on a surface but can at a degenerate sample. Fall back to
            // "away from the centroid, in the tangent plane".
            Vec3d radial = m_path[i].pos - c;
            radial -= radial.dot(nrm) * nrm;
            b = radial;
        }
        m_binormal[i] = safe_normalize(b, Vec3d::UnitX());
    }

    if (m_closed) {
        // WINDING DECIDES, not drag direction. The outward binormal must point
        // away from the loop's interior, so flip the WHOLE field (not per sample -
        // that is what tears the strip) when the average radial agreement says it
        // points inward. Summing over every sample rather than testing one makes
        // the decision robust to a single sample near the centroid.
        double agree = 0.0;
        for (size_t i = 0; i < n; ++ i) {
            Vec3d radial = m_path[i].pos - c;
            const Vec3d& nrm = m_path[i].normal;
            radial -= radial.dot(nrm) * nrm; // in the surface's tangent plane
            agree += m_binormal[i].dot(radial);
        }
        if (agree < 0.0)
            for (Vec3d& b : m_binormal)
                b = -b;
    }
    else {
        // Open: no interior, so the sign is PARALLEL-TRANSPORTED from the first
        // sample - pick the branch that keeps b_i . b_{i-1} > 0. A pointwise
        // t x n flips across an inflection and would tear the strip in half.
        for (size_t i = 1; i < n; ++ i)
            if (m_binormal[i].dot(m_binormal[i - 1]) < 0.0)
                m_binormal[i] = -m_binormal[i];
    }
}

Vec3d DrawCutStroke::binormal(size_t i) const
{
    return i < m_binormal.size() ? m_binormal[i] : Vec3d::UnitX();
}

// ---------------------------------------------------------------------------
// Self-crossing
// ---------------------------------------------------------------------------

// Best-fit plane of the path, as (origin, two orthonormal in-plane axes). Newell's
// method for the normal: it is the area-weighted average of the face normals of
// the fan, which is stable for a nearly planar ring and degrades gracefully for a
// wobbly one.
static void stroke_plane(const DrawCutStroke& stroke, Vec3d& origin, Vec3d& ax, Vec3d& ay)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    origin = stroke.centroid();

    Vec3d nrm = Vec3d::Zero();
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d& a = p[i].pos;
        const Vec3d& b = p[(i + 1) % n].pos;
        nrm.x() += (a.y() - b.y()) * (a.z() + b.z());
        nrm.y() += (a.z() - b.z()) * (a.x() + b.x());
        nrm.z() += (a.x() - b.x()) * (a.y() + b.y());
    }
    if (nrm.norm() < 1e-12) {
        // A straight stroke has no enclosed area, so Newell gives nothing. Any
        // plane containing the stroke will do: take the average surface normal.
        nrm = Vec3d::Zero();
        for (const DrawCutSample& s : p)
            nrm += s.normal;
    }
    nrm = safe_normalize(nrm, Vec3d::UnitZ());

    // An in-plane basis. Pick the world axis least parallel to the normal so the
    // cross product is well conditioned.
    Vec3d seed = std::abs(nrm.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    ax = safe_normalize(seed - seed.dot(nrm) * nrm, Vec3d::UnitX());
    ay = nrm.cross(ax);
}

// Do segments (a,b) and (c,d) properly cross, in 2D? Touching at an endpoint does
// not count - adjacent segments of a polyline always do that.
static bool segments_cross(const Vec2d& a, const Vec2d& b, const Vec2d& c, const Vec2d& d)
{
    auto cross = [](const Vec2d& u, const Vec2d& v) { return u.x() * v.y() - u.y() * v.x(); };
    const Vec2d r = b - a;
    const Vec2d s = d - c;
    const double denom = cross(r, s);
    if (std::abs(denom) < 1e-12)
        return false; // parallel or collinear: a collinear overlap is not a crossing
    const double t = cross(c - a, s) / denom;
    const double u = cross(c - a, r) / denom;
    const double eps = 1e-9;
    return t > eps && t < 1.0 - eps && u > eps && u < 1.0 - eps;
}

bool draw_cut_self_crossing(const DrawCutStroke& stroke)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    if (n < 4)
        return false;

    Vec3d origin, ax, ay;
    stroke_plane(stroke, origin, ax, ay);

    std::vector<Vec2d> flat(n);
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d d = p[i].pos - origin;
        flat[i] = Vec2d(d.dot(ax), d.dot(ay));
    }

    const size_t n_seg = stroke.is_closed() ? n : n - 1;
    for (size_t i = 0; i + 1 < n_seg; ++ i) {
        const Vec2d& a = flat[i];
        const Vec2d& b = flat[(i + 1) % n];
        for (size_t j = i + 2; j < n_seg; ++ j) {
            // Adjacent segments share an endpoint by construction; so do the first
            // and the last of a closed ring.
            if (stroke.is_closed() && i == 0 && j + 1 == n_seg)
                continue;
            const Vec2d& c = flat[j];
            const Vec2d& d = flat[(j + 1) % n];
            if (segments_cross(a, b, c, d))
                return true;
        }
    }
    return false;
}

bool draw_cut_strip_folds(const DrawCutStroke& stroke, double extension, double* worst_kappa)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    if (worst_kappa != nullptr)
        *worst_kappa = 0.0;
    if (n < 3 || extension <= 0.0)
        return false;

    double worst = 0.0;
    const size_t first = stroke.is_closed() ? 0 : 1;
    const size_t last  = stroke.is_closed() ? n : n - 1;
    for (size_t i = first; i < last; ++ i) {
        const Vec3d& a = p[(i + n - 1) % n].pos;
        const Vec3d& b = p[i % n].pos;
        const Vec3d& c = p[(i + 1) % n].pos;

        // Discrete curvature from the circumradius of the three points:
        // kappa = 4 * area / (|ab| |bc| |ca|).
        const double ab = (b - a).norm();
        const double bc = (c - b).norm();
        const double ca = (a - c).norm();
        if (ab < 1e-12 || bc < 1e-12 || ca < 1e-12)
            continue;
        const double area = 0.5 * (b - a).cross(c - a).norm();
        const double kappa = 4.0 * area / (ab * bc * ca);

        // THE SIGN MATTERS, and it is the opposite of the obvious guess. The
        // outward rail is the stroke pushed out by E along the binormal, so it
        // FOLDS where the stroke's own turn centre is on the OUTWARD side - a
        // CONCAVE corner as seen from outside - because there the outward offset
        // is walking toward a centre only 1/kappa away and overshoots it once
        // E * kappa > 1. Where the turn centre is on the INWARD side (a convex
        // corner, which is every point of a circle drawn as a loop) the outward
        // offset moves AWAY from the centre and the rail simply gets longer: it
        // cannot fold at all, however tight the curve.
        //
        // The turn vector (the change in unit tangent) points toward the turn
        // centre, so "the centre is on the outward side" is turn . b > 0... with
        // b the OUTWARD binormal, which for a closed loop points away from the
        // interior. A circle therefore scores zero here, which is correct.
        const Vec3d turn = (c - b).normalized() - (b - a).normalized();
        if (turn.dot(stroke.binormal(i % n)) > 0.0)
            worst = std::max(worst, kappa);
    }

    if (worst_kappa != nullptr)
        *worst_kappa = worst;
    return extension * worst > 1.0;
}

// ---------------------------------------------------------------------------
// The cutter solid
// ---------------------------------------------------------------------------

// The inward ray at path sample i, unit length, pointing INTO the part.
static Vec3d inward_dir(const DrawCutStroke& stroke, const DrawCutParams& params, size_t i)
{
    switch (params.direction) {
    case DrawCutDirection::View:  return safe_normalize(params.view_dir, -Vec3d::UnitZ());
    case DrawCutDirection::AxisX: return -Vec3d::UnitX();
    case DrawCutDirection::AxisY: return -Vec3d::UnitY();
    case DrawCutDirection::AxisZ: return -Vec3d::UnitZ();
    default: break;
    }
    // Surface normal, angle 0: straight in along the INWARD normal. Phase 2
    // rotates this toward the binormal by the draft angle.
    return -stroke.path()[i].normal;
}

indexed_triangle_set draw_cut_cutter_solid(const DrawCutStroke& stroke,
                                           const DrawCutParams& params,
                                           const BoundingBoxf3& bbox,
                                           double               face_offset)
{
    indexed_triangle_set its;
    if (!stroke.valid())
        return its;

    const std::vector<DrawCutSample>& p = stroke.path();
    const bool   closed = stroke.is_closed();
    const double ext    = std::max(0.0, params.extension);

    // Through-all reaches 1.05 * the bounding box diagonal, the same "reach past
    // the object" slack curved_cut_split() uses for its slab floor. Anything less
    // and a cut aimed diagonally through a long part stops inside it.
    const double diag  = bbox.defined ? bbox.size().norm() : 100.0;
    const double depth = params.through_all ? 1.05 * diag + 1.0 : std::max(0.01, params.depth);

    // THE RAILS, and the one thing about them that is easy to get wrong.
    //
    // Each sample contributes two points - one outside the part, one inside it -
    // and the strip between them is the cut surface. The ruling is a STRAIGHT LINE
    // ALONG d_i, the cut direction, so both points lie on that line:
    //
    //   out_i = p_i - E * d_i     (back OUT along the cut direction, clear of the
    //                              surface: E doubles as how far above the face the
    //                              surface starts, so a stroke drawn in a concavity
    //                              has its rim in free air rather than buried in
    //                              material where the boolean would leave a skin)
    //   in_i  = p_i + D * d_i     (in by Depth, or through the bbox)
    //
    // What the outward point must NOT do is move sideways - out along the binormal.
    // That was the first version here, reading the spec's "E is how far the surface
    // reaches past the stroke" as a lateral push, and it silently turned every
    // closed cut into a DRAFT: the strip then runs from radius r + E at the top to
    // radius r at the bottom, so a 12 mm circle with 5 mm extension cut a truncated
    // cone of 1.65x the intended volume rather than a cylinder. At angle 0 - all of
    // phase 1 - there is no draft, so the ruling is straight along d and E only
    // ever measures ALONG that line. (Phase 2's draft angle is what tilts d itself,
    // toward the binormal, which is where the binormal earns its keep.)
    //
    // "Reaching past the stroke" for an OPEN stroke is the TANGENT extension at the
    // two ends - and that is what lets the surface get past the silhouette so the
    // boolean actually separates the part.
    struct Rail { Vec3d out, in; };
    std::vector<Rail> rails;
    const size_t n = p.size();
    rails.reserve(n + 2);

    // THE KERF OFFSET AND ITS SIGN.
    //
    // `face_offset` comes from curved_cut_thickness_faces(), which hands back
    // lo <= 0 <= hi along "the cut normal". Here that normal is the STRIP's own -
    // t x d, so the band is measured along the local frame rather than along a
    // global Z, which is the rule phase 4 established for connectors and which
    // matters wherever the strip is not vertical.
    //
    // The SIGN has to be pinned to which side the cutter's interior is on, or the
    // kerf grows the halves instead of shrinking them. t x d has an arbitrary
    // per-sample sign (it flips with the drag direction and across an inflection),
    // so it cannot be used raw:
    //
    //   OPEN stroke: the cutter is the drawn surface swept along +sweep, so its
    //     interior is on the +sweep side and the surface must move by -offset *
    //     sweep - i.e. the INTERSECTION side (lo, negative) shrinks, and so does
    //     the A_NOT_B side (hi, positive).
    //   CLOSED stroke: the cutter is a tube whose interior is the plug, so the wall
    //     must move toward the plug's axis - along -binormal, the binormal being the
    //     direction that points away from the loop's interior by construction.
    //
    // Getting this backwards is invisible without a kerf and shows up with one as
    // "the two halves together are BIGGER than the part", which is what the
    // volume-difference test measures.
    //
    // `sweep` is also what the open case's slab is built along, below, so it is
    // derived here once for both jobs. It is ONE direction for the whole strip, not
    // the per-sample strip normal: a per-sample sweep of a curved stroke folds
    // wherever the stroke turns and the resulting solid self-intersects.
    Vec3d sweep = Vec3d::Zero();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d d  = inward_dir(stroke, params, i);
        Vec3d       sn = stroke.tangent(i).cross(d);
        if (sn.norm() < 1e-9)
            continue;
        sn.normalize();
        // Keep the sum coherent: flip a sample whose normal opposes the running
        // total, or a stroke that doubles back would cancel itself to nothing.
        if (!sweep.isZero() && sn.dot(sweep) < 0.0)
            sn = -sn;
        sweep += sn;
    }
    sweep = safe_normalize(sweep, Vec3d::UnitY());

    auto push_rail = [&](const Vec3d& pos, const Vec3d& t, const Vec3d& d, const Vec3d& b) {
        // Away from the cutter's interior, so a POSITIVE offset always grows the
        // cutter and a negative one always shrinks it - see above.
        const Vec3d out_of_cutter = closed ? Vec3d(-b) : Vec3d(-sweep);
        const Vec3d shift = face_offset * out_of_cutter;
        Rail r;
        r.out = pos - ext * d + shift;
        r.in  = pos + depth * d + shift;
        rails.push_back(r);
    };

    if (!closed) {
        // Leading end, extended BACKWARDS along the tangent so the surface reaches
        // past the silhouette on that side.
        const Vec3d t0 = stroke.tangent(0);
        push_rail(p.front().pos - ext * t0, t0, inward_dir(stroke, params, 0), stroke.binormal(0));
    }

    for (size_t i = 0; i < n; ++ i)
        push_rail(p[i].pos, stroke.tangent(i), inward_dir(stroke, params, i), stroke.binormal(i));

    if (!closed) {
        const Vec3d tN = stroke.tangent(n - 1);
        push_rail(p.back().pos + ext * tN, tN, inward_dir(stroke, params, n - 1), stroke.binormal(n - 1));
    }

    const size_t m = rails.size();
    if (m < 3)
        return its;

    // ----------------------------------------------------------------------
    // FROM A STRIP TO A SOLID, and the trap that is easy to walk into twice.
    //
    // A boolean needs a solid. The strip between the two rails is a SURFACE, and
    // for a CLOSED stroke that is already enough: the band is a tube, so capping
    // its two boundary rings gives a solid with a real interior - the plug.
    //
    // For an OPEN stroke it is not enough, and "close the one boundary loop with a
    // fan" - the obvious repair - produces a solid of ZERO VOLUME: a flattened
    // bag, watertight by every edge count and empty inside. The INTERSECTION then
    // comes back with nothing and the caller sees one half, which is the "an open
    // line across a box gave back 1.5x the box" symptom.
    //
    // The fix is the one curved_cut_lower_slab() already uses for the sheet: sweep
    // the surface sideways to a FAR BOUNDARY well outside the part, so the solid is
    // "everything on one side of the drawn surface". Here the sweep is along the
    // strip's own normal (t x d), and the reach is the bbox diagonal with the same
    // 1.05 slack. Intersecting with that gives one whole half of the part; A_NOT_B
    // gives the other.
    // ----------------------------------------------------------------------

    if (closed) {
        // Vertices: the outward ring then the inward ring, so index arithmetic is
        // trivial - the same layout curved_cut_lower_slab() uses for its two grids.
        its.vertices.reserve(m * 2 + 2);
        for (const Rail& r : rails)
            its.vertices.emplace_back(r.out.cast<float>());
        for (const Rail& r : rails)
            its.vertices.emplace_back(r.in.cast<float>());

        auto O = [](size_t i) { return int(i); };
        auto I = [m](size_t i) { return int(m + i); };

        // The tube wall: a quad grid between the two rails, two triangles per span,
        // exactly the way curved_cut_lower_slab() stitches its rim.
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(O(i), I(i), I(j)));
            its.indices.emplace_back(Vec3i32(O(i), I(j), O(j)));
        }

        // Cap the two boundary rings from their own centroids, which lie inside them
        // for any stroke that is not self-crossing - and a self-crossing one is
        // refused before it gets here. The wall walks the out ring as O(j) -> O(i)
        // and the in ring as I(i) -> I(j), so each cap has to use its ring the other
        // way round or the shared edge ends up traversed twice the same way (which
        // every edge count still calls watertight, and which Manifold rejects).
        Vec3d co = Vec3d::Zero(), ci = Vec3d::Zero();
        for (const Rail& r : rails) { co += r.out; ci += r.in; }
        co /= double(m);
        ci /= double(m);
        const int c_out = int(its.vertices.size());
        its.vertices.emplace_back(co.cast<float>());
        const int c_in = int(its.vertices.size());
        its.vertices.emplace_back(ci.cast<float>());
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(c_out, O(i), O(j)));
            its.indices.emplace_back(Vec3i32(c_in,  I(j), I(i)));
        }
    }
    else {
        // The sweep distance: far enough to leave the part on the swept side, so the
        // solid really is a half-space as far as this object is concerned.
        const double reach = 1.05 * diag + 1.0;

        // `sweep` was derived above, where the kerf offset's sign needed it too.
        const Vec3d shift = reach * sweep;

        // Two copies of the strip - the drawn one and the swept one - and the rim
        // between them, which is the same two-grids-plus-rim construction
        // curved_cut_lower_slab() uses. 4m vertices: out, in, out+shift, in+shift.
        its.vertices.reserve(m * 4);
        for (const Rail& r : rails) its.vertices.emplace_back(r.out.cast<float>());
        for (const Rail& r : rails) its.vertices.emplace_back(r.in.cast<float>());
        for (const Rail& r : rails) its.vertices.emplace_back(Vec3f((r.out + shift).cast<float>()));
        for (const Rail& r : rails) its.vertices.emplace_back(Vec3f((r.in  + shift).cast<float>()));

        auto A = [](size_t i) { return int(i); };            // out, drawn face
        auto B = [m](size_t i) { return int(m + i); };       // in,  drawn face
        auto A2 = [m](size_t i) { return int(2 * m + i); };  // out, swept face
        auto B2 = [m](size_t i) { return int(3 * m + i); };  // in,  swept face

        // The drawn face (the cut surface itself) and the swept face, wound
        // oppositely so the solid between them is the interior.
        for (size_t i = 0; i + 1 < m; ++ i) {
            const size_t j = i + 1;
            its.indices.emplace_back(Vec3i32(A(i), B(i), B(j)));
            its.indices.emplace_back(Vec3i32(A(i), B(j), A(j)));
            its.indices.emplace_back(Vec3i32(A2(i), B2(j), B2(i)));
            its.indices.emplace_back(Vec3i32(A2(i), A2(j), B2(j)));
        }
        // The rim, on all four edges of the strip: the out curve, the in curve, and
        // the two end segments.
        for (size_t i = 0; i + 1 < m; ++ i) {
            const size_t j = i + 1;
            // out curve
            its.indices.emplace_back(Vec3i32(A(i), A(j), A2(j)));
            its.indices.emplace_back(Vec3i32(A(i), A2(j), A2(i)));
            // in curve
            its.indices.emplace_back(Vec3i32(B(i), B2(i), B2(j)));
            its.indices.emplace_back(Vec3i32(B(i), B2(j), B(j)));
        }
        // The two ends.
        its.indices.emplace_back(Vec3i32(A(0), A2(0), B2(0)));
        its.indices.emplace_back(Vec3i32(A(0), B2(0), B(0)));
        its.indices.emplace_back(Vec3i32(A(m - 1), B(m - 1), B2(m - 1)));
        its.indices.emplace_back(Vec3i32(A(m - 1), B2(m - 1), A2(m - 1)));
    }

    // WINDING. The solid above is built consistently, but whether it comes out
    // wound OUTWARDS depends on the stroke's own handedness - and cut_with_solid()
    // decides "inside" from face orientation, so a solid wound inwards would have
    // its INTERSECTION come back as the complement. its_volume() is the same test
    // cut_with_solid() applies to the object; apply it to the cutter here so the
    // cutter handed over is always outward-wound, whichever way the user dragged.
    if (its_volume(its) < 0.f)
        for (Vec3i32& t : its.indices)
            std::swap(t(1), t(2));

    return its;
}

// ---------------------------------------------------------------------------
// Empty sides
// ---------------------------------------------------------------------------

// Is `pt` inside the closed triangle soup `solid`? Parity of the crossings of a ray
// from pt. The cutter is a few thousand faces, so this is cheap, and the answer only
// has to be right for the vertices of a real mesh - a vertex exactly ON the cutter's
// surface can go either way and the caller does not care, since such a vertex is on
// the cut face.
//
// The ray direction is NOT an axis. An axis ray runs exactly along, or exactly
// through the shared edges of, the axis-aligned faces this cutter is full of (a
// stroke drawn on a cube's flat top face gives a cutter with faces parallel to +Z
// everywhere), and every one of those is a parity answer that could go either way -
// which showed up as "an open line right across the cube reports a side empty". A
// direction with no small integer ratios between its components misses every such
// degeneracy for any geometry a user will produce.
static bool point_in_solid(const indexed_triangle_set& solid, const Vec3d& pt)
{
    // An "irrational-ish" direction, normalised. Fixed rather than random so the
    // answer is deterministic, which a cut has to be.
    static const Vec3d dir = Vec3d(0.3131592653, 0.4271828182, 0.8481471805).normalized();

    // A basis with `dir` as its third axis, so the in-triangle test below is the
    // same barycentric solve an axis ray would use, just in the rotated frame.
    const Vec3d ax = (std::abs(dir.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY());
    const Vec3d e0 = (ax - ax.dot(dir) * dir).normalized();
    const Vec3d e1 = dir.cross(e0);

    int crossings = 0;
    for (const Vec3i32& tri : solid.indices) {
        // Each vertex as (u, v, w): u,v across the ray, w along it.
        Vec3d uvw[3];
        for (int k = 0; k < 3; ++ k) {
            const Vec3d rel = solid.vertices[tri(k)].cast<double>() - pt;
            uvw[k] = Vec3d(rel.dot(e0), rel.dot(e1), rel.dot(dir));
        }

        // Barycentric solve at (u, v) == (0, 0), i.e. on the ray's own axis. A
        // triangle seen edge-on projects to zero area and is skipped: a neighbouring
        // non-degenerate face provides the same crossing.
        const double det = (uvw[1].y() - uvw[2].y()) * (uvw[0].x() - uvw[2].x()) +
                           (uvw[2].x() - uvw[1].x()) * (uvw[0].y() - uvw[2].y());
        if (std::abs(det) < 1e-12)
            continue;
        const double l0 = ((uvw[1].y() - uvw[2].y()) * (-uvw[2].x()) + (uvw[2].x() - uvw[1].x()) * (-uvw[2].y())) / det;
        const double l1 = ((uvw[2].y() - uvw[0].y()) * (-uvw[2].x()) + (uvw[0].x() - uvw[2].x()) * (-uvw[2].y())) / det;
        const double l2 = 1.0 - l0 - l1;
        if (l0 < 0.0 || l1 < 0.0 || l2 < 0.0)
            continue;

        // Only crossings in FRONT of the point count.
        if (l0 * uvw[0].z() + l1 * uvw[1].z() + l2 * uvw[2].z() > 0.0)
            ++ crossings;
    }
    return (crossings & 1) != 0;
}

void draw_cut_empty_sides(const indexed_triangle_set& mesh,
                          const DrawCutStroke&        stroke,
                          const DrawCutParams&        params,
                          bool&                       upper_empty,
                          bool&                       lower_empty)
{
    upper_empty = true;
    lower_empty = true;
    if (mesh.empty() || !stroke.valid())
        return;

    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(std::max(0.0, params.thickness), params.thickness_offset, face_lo, face_hi);
    const bool kerf = params.thickness > 0.0;

    const indexed_triangle_set cutter_lo = draw_cut_cutter_solid(stroke, params, bbox, face_lo);
    if (cutter_lo.empty())
        return;
    const indexed_triangle_set cutter_hi = kerf ? draw_cut_cutter_solid(stroke, params, bbox, face_hi)
                                               : indexed_triangle_set();

    // "Inside the cutter" is the INTERSECTION side, which cut_with_solid() calls
    // `lower`. draw_cut_split() then swaps the pair for a closed stroke (the plug
    // is the upper half), so the swap has to happen here too or the panel would
    // name the wrong side.
    //
    // COST. Each test is one parity ray against the cutter's faces, and the loop
    // short-circuits as soon as both sides have a vertex - which on a cut that works
    // is the first handful of vertices. The bad case is the cut that does NOT work:
    // then one side never fills in and every vertex is tested, which on a 500k-vertex
    // model against a few thousand cutter faces is a visible stall - and this runs on
    // every parameter change, from a slider being dragged.
    //
    // So the walk is STRIDED: it visits at most MaxProbes vertices, spread evenly
    // over the mesh rather than taken from the front (the front of an STL's vertex
    // list is one corner of the part, which would answer for that corner only). The
    // test stays conservative in the direction that matters - it can only ever claim
    // a side is NON-empty, which requires actually finding a vertex there - so a
    // stride can produce a false "empty", never a false "has material". A false
    // "empty" on a part whose only material on one side is a feature smaller than one
    // stride is the trade, and the panel's warning is advisory in that direction: it
    // says "this line does not separate the part", which is the safe thing to say
    // about a cut leaving a sliver.
    static const size_t MaxProbes = 20000;
    const size_t n_verts = mesh.vertices.size();
    const size_t stride  = n_verts > MaxProbes ? (n_verts + MaxProbes - 1) / MaxProbes : 1;

    bool inside_empty = true, outside_empty = true;
    for (size_t i = 0; i < n_verts; i += stride) {
        const Vec3d pt = mesh.vertices[i].cast<double>();
        if (inside_empty && point_in_solid(cutter_lo, pt))
            inside_empty = false;
        if (outside_empty && !point_in_solid(kerf ? cutter_hi : cutter_lo, pt))
            outside_empty = false;
        if (!inside_empty && !outside_empty)
            break;
    }

    if (stroke.is_closed()) {
        upper_empty = inside_empty;   // the plug
        lower_empty = outside_empty;  // the remainder
    }
    else {
        lower_empty = inside_empty;
        upper_empty = outside_empty;
    }
}

// ---------------------------------------------------------------------------
// The split
// ---------------------------------------------------------------------------

bool draw_cut_split(const indexed_triangle_set& mesh,
                    const DrawCutStroke&        stroke,
                    const DrawCutParams&        params,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    DrawCutError*               err)
{
    auto fail = [err](DrawCutError e) {
        if (err != nullptr)
            *err = e;
        return false;
    };
    if (err != nullptr)
        *err = DrawCutError::None;

    if (mesh.empty())
        return fail(DrawCutError::CutterDegenerate);
    if (!stroke.valid())
        return fail(stroke.error() == DrawCutError::None ? DrawCutError::TooShort : stroke.error());

    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    const double t = std::max(0.0, params.thickness);
    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(t, params.thickness_offset, face_lo, face_hi);
    const bool kerf = t > 0.0;

    // THE KERF, the same shape the curved cut uses: with a thickness the two
    // halves are cut by DIFFERENT solids - one displaced by face_lo along the
    // strip's own normal, the other by face_hi - so the band between them is
    // removed from both. At t == 0 both offsets are zero and ONE solid is built,
    // which is why the no-kerf output is not "close to" what it was without the
    // kerf code, it is the same.
    const indexed_triangle_set cutter_lo = draw_cut_cutter_solid(stroke, params, bbox, face_lo);
    if (cutter_lo.empty())
        return fail(DrawCutError::CutterDegenerate);
    const indexed_triangle_set cutter_hi = kerf ? draw_cut_cutter_solid(stroke, params, bbox, face_hi)
                                               : indexed_triangle_set();
    if (kerf && cutter_hi.empty())
        return fail(DrawCutError::CutterDegenerate);

    // A FOLD is advisory: Manifold tolerates a slightly self-intersecting cutter
    // and mcut is the fallback, so a fold degrades to "boolean failed ->
    // complement recovery" rather than to a wrong cut. Log it so the reason is
    // recoverable from a log when a cut does come out strange.
    double kappa = 0.0;
    if (draw_cut_strip_folds(stroke, params.extension, &kappa))
        BOOST_LOG_TRIVIAL(warning) << "Draw cut: the ruled strip folds near a tight corner (extension "
                                   << params.extension << " mm, curvature " << kappa
                                   << " 1/mm); the cut may be imprecise there";

    // WHICH SIDE IS UPPER. cut_with_solid() produces (inside, outside) as
    // (lower, upper):
    //
    //   OPEN stroke: that IS the convention we want - the strip cuts the part in
    //     two with no inside or outside, and the plane's own +Z convention
    //     applies, which is what cut_with_solid()'s A_NOT_B side already means for
    //     the flat and curved cuts.
    //   CLOSED stroke: the cutter encloses a PLUG, and the plug - the INTERSECTION -
    //     is what the stroke drew around, so it is the UPPER half. Swap the pair.
    //
    // Doing the swap here rather than in the caller is what lets the gizmo and the
    // tests treat open and closed strokes identically.
    indexed_triangle_set  inside, outside;
    indexed_triangle_set* p_inside  = nullptr;
    indexed_triangle_set* p_outside = nullptr;
    if (stroke.is_closed()) {
        p_inside  = upper != nullptr ? &inside  : nullptr;
        p_outside = lower != nullptr ? &outside : nullptr;
    }
    else {
        p_inside  = lower != nullptr ? &inside  : nullptr;
        p_outside = upper != nullptr ? &outside : nullptr;
    }

    const bool ok = cut_with_solid(mesh, cutter_lo, kerf ? cutter_hi : cutter_lo, kerf,
                                   p_outside, p_inside, "Draw cut");

    if (stroke.is_closed()) {
        if (upper != nullptr) *upper = std::move(inside);
        if (lower != nullptr) *lower = std::move(outside);
    }
    else {
        if (lower != nullptr) *lower = std::move(inside);
        if (upper != nullptr) *upper = std::move(outside);
    }

    if (!ok)
        return fail(DrawCutError::EmptySide);
    return true;
}

} // namespace Slic3r
