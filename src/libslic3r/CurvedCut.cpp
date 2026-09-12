#include "CurvedCut.hpp"
#include "MeshBoolean.hpp"
#include "MeshSculpt.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

const int CurvedCutSheet::MinResolution;
const int CurvedCutSheet::MaxResolution;
const int CurvedCutSheet::DefaultResolution;
const int CurvedCutSheet::DefaultSamples;
const int CurvedCutSheet::CutSamples;

// ---------------------------------------------------------------------------
// Control grid
// ---------------------------------------------------------------------------

void CurvedCutSheet::publish_reference()
{
    m_ref_z           = m_z;
    m_ref_half_size_u = m_half_size_u;
    m_ref_half_size_v = m_half_size_v;
}

void CurvedCutSheet::commit_reference() { publish_reference(); }

void CurvedCutSheet::reset(int resolution)
{
    // The one-int form is the SQUARE form: it sets both counts, which is what it
    // has always meant. A non-positive argument keeps the grid as it is.
    if (resolution > 0)
        reset(resolution, resolution);
    else {
        m_z.assign(size_t(m_nx) * size_t(m_ny), 0.0);
        publish_reference();
    }
}

void CurvedCutSheet::reset(int nx, int ny)
{
    if (nx > 0)
        m_nx = std::clamp(nx, MinResolution, MaxResolution);
    if (ny > 0)
        m_ny = std::clamp(ny, MinResolution, MaxResolution);
    m_z.assign(size_t(m_nx) * size_t(m_ny), 0.0);
    publish_reference();
}

void CurvedCutSheet::set_values(const std::vector<double>& z)
{
    if (z.size() == m_z.size()) {
        m_z = z;
        publish_reference();
    }
}

// A 180-degree turn of the plane frame about its own X: local y and z both
// negate, so the height field must become f_new(x, -y) = -f_old(x, y). On the
// control grid that is "mirror j, negate the value" - exact, and self-inverse.
void CurvedCutSheet::flip_about_u()
{
    // Mirrors ALONG V, so the mirrored index runs over NY. The row stride is NX.
    // Swapping those two counts is silent on a square grid and out of bounds on
    // a 10 x 2 one.
    std::vector<double> nz(m_z.size(), 0.0);
    for (int j = 0; j < m_ny; ++ j)
        for (int i = 0; i < m_nx; ++ i)
            nz[size_t(j) * size_t(m_nx) + size_t(i)] = -m_z[size_t(m_ny - 1 - j) * size_t(m_nx) + size_t(i)];
    m_z = std::move(nz);
    // A flip IS an edit of the surface, so the reference follows it. Leaving the
    // pre-flip reference in place would let the next extent re-fit re-sample the
    // OLD surface and quietly undo the flip.
    publish_reference();
}

// The same for a turn about the frame's Y: local x and z negate, so mirror i.
void CurvedCutSheet::flip_about_v()
{
    // ... and this one mirrors along u, over NX.
    std::vector<double> nz(m_z.size(), 0.0);
    for (int j = 0; j < m_ny; ++ j)
        for (int i = 0; i < m_nx; ++ i)
            nz[size_t(j) * size_t(m_nx) + size_t(i)] = -m_z[size_t(j) * size_t(m_nx) + size_t(m_nx - 1 - i)];
    m_z = std::move(nz);
    publish_reference();
}

Vec2d CurvedCutSheet::control_xy(int i, int j) const
{
    // control_v(j), not control_u(j): the two axes have their own counts now.
    return Vec2d((2.0 * control_u(i) - 1.0) * m_half_size_u,
                 (2.0 * control_v(j) - 1.0) * m_half_size_v);
}

void CurvedCutSheet::set_half_size(double hs_u, double hs_v, bool resample)
{
    hs_u = std::max(hs_u, 1e-6);
    hs_v = std::max(hs_v, 1e-6);
    if (hs_u == m_half_size_u && hs_v == m_half_size_v)
        return;

    if (!resample || is_flat()) {
        m_half_size_u = hs_u;
        m_half_size_v = hs_v;
        // A stretch (resample == false) IS an edit of the surface - it takes the
        // control values with it - so the reference follows. A flat sheet has
        // nothing to lose either way.
        publish_reference();
        return;
    }

    // Keep the SURFACE fixed in the plane, not the control values: each new
    // control point takes the REFERENCE surface's height at the same local (x,y)
    // in mm. evaluate_local() clamps outside the reference domain, so a point
    // that ends up beyond that rectangle picks up the border value rather than
    // an extrapolated overshoot - the same clamped-boundary rule evaluate() uses.
    //
    // PHASE 3: reading the REFERENCE, not the live values, is what makes this
    // idempotent. Re-sampling is a lossy round trip through Catmull-Rom whenever
    // the grid phase changes, and feeding each re-sample its predecessor's output
    // compounds that loss without bound over a plane drag. Reading the reference
    // makes every re-fit a single hop from the shape the user actually drew: two
    // re-fits to the same extent give the same values, and coming back to an
    // earlier extent reproduces it exactly.
    // THE STALE REFERENCE. at() hands out a mutable reference, so a caller can
    // change the surface without going through any of the editing entry points
    // that republish - the tests do exactly that, and so could future code. Catch
    // it here: if the live values are NOT what the reference produces at the
    // CURRENT extent, the surface has been edited behind our back and the live
    // values are the truth. Republish before re-sampling, or the re-sample would
    // resurrect a surface the caller has already replaced.
    //
    // The check is exact equality against the values the reference was published
    // with (the extent has not moved since, or a previous re-sample updated
    // neither), so it costs one vector compare and never fires spuriously.
    if (m_ref_z.size() != m_z.size() ||
        (m_ref_half_size_u == m_half_size_u && m_ref_half_size_v == m_half_size_v && m_ref_z != m_z))
        publish_reference();

    std::vector<double> nz(size_t(m_nx) * size_t(m_ny), 0.0);
    if (m_ref_z.size() == m_z.size()) {
        // Evaluate the reference surface: temporarily wear the reference values
        // and extent, sample, then put the new ones on. (evaluate_local() reads
        // m_z / m_half_size_*, and the reference is the same grid resolution, so
        // this is a swap rather than a second evaluator.)
        std::vector<double> live_z    = std::move(m_z);
        const double        live_hs_u = m_half_size_u;
        const double        live_hs_v = m_half_size_v;
        m_z           = m_ref_z;
        m_half_size_u = m_ref_half_size_u;
        m_half_size_v = m_ref_half_size_v;
        for (int j = 0; j < m_ny; ++ j) {
            const double y = (2.0 * control_v(j) - 1.0) * hs_v;
            for (int i = 0; i < m_nx; ++ i) {
                const double x = (2.0 * control_u(i) - 1.0) * hs_u;
                nz[size_t(j) * size_t(m_nx) + size_t(i)] = evaluate_local(x, y);
            }
        }
        m_z           = std::move(live_z);
        m_half_size_u = live_hs_u;
        m_half_size_v = live_hs_v;
    }
    else {
        // No usable reference (a resolution change left it stale) - fall back to
        // the live surface, which is what phase 2 always did.
        for (int j = 0; j < m_ny; ++ j) {
            const double y = (2.0 * control_v(j) - 1.0) * hs_v;
            for (int i = 0; i < m_nx; ++ i) {
                const double x = (2.0 * control_u(i) - 1.0) * hs_u;
                nz[size_t(j) * size_t(m_nx) + size_t(i)] = evaluate_local(x, y);
            }
        }
    }
    m_half_size_u = hs_u;
    m_half_size_v = hs_v;
    m_z           = std::move(nz);
    // NOT publish_reference(): the whole point is that the reference outlives the
    // re-sample. A later edit republishes it.
}

Vec3d CurvedCutSheet::control_pos(int i, int j) const
{
    const Vec2d xy = control_xy(i, j);
    return Vec3d(xy.x(), xy.y(), at(i, j));
}

bool CurvedCutSheet::is_flat() const
{
    for (double z : m_z)
        if (z != 0.0)
            return false;
    return true;
}

double CurvedCutSheet::max_displacement() const
{
    double m = 0.0;
    for (double z : m_z)
        m = std::max(m, std::abs(z));
    return m;
}

// ---------------------------------------------------------------------------
// Catmull-Rom evaluation
// ---------------------------------------------------------------------------

// One-dimensional uniform Catmull-Rom through p1,p2 with p0/p3 as the tangent
// neighbours; t in [0,1] runs from p1 to p2.
static inline double catmull_rom(double p0, double p1, double p2, double p3, double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

double CurvedCutSheet::evaluate(double u, double v) const
{
    if (m_nx < 2 || m_ny < 2 || m_z.empty())
        return 0.0;
    // A flat grid is flat everywhere - short-circuit so the invariant that
    // zero displacement gives exactly z == 0 does not depend on float maths.
    if (is_flat())
        return 0.0;

    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    // Each axis spans its OWN count of cells. The Catmull-Rom evaluation below
    // is already separable - four row interpolations along u, then one along v -
    // so a rectangular grid needs nothing but the right extent per axis.
    const double fu = u * double(m_nx - 1);
    const double fv = v * double(m_ny - 1);
    int          iu = int(std::floor(fu));
    int          iv = int(std::floor(fv));
    iu = std::clamp(iu, 0, m_nx - 2);
    iv = std::clamp(iv, 0, m_ny - 2);
    const double tu = fu - double(iu);
    const double tv = fv - double(iv);

    // Clamped boundary: the neighbour outside the grid repeats the edge row,
    // which gives a zero second derivative at the border rather than an
    // extrapolated overshoot.
    auto z_at = [this](int i, int j) -> double {
        return at(std::clamp(i, 0, m_nx - 1), std::clamp(j, 0, m_ny - 1));
    };

    // A 2-POINT AXIS INTERPOLATES LINEARLY, not through the clamped spline.
    //
    // The obvious reading - "with only two rows both outer neighbours clamp onto
    // the two real ones, so Catmull-Rom degenerates to linear" - is FALSE, and it
    // is worth spelling out because the whole point of a 10 x 2 grid is that it
    // be RULED. catmull_rom(a, a, b, b, t) works out to
    //
    //     a + (b - a) * (0.5 t + 1.5 t^2 - t^3)
    //
    // whose blend factor is not t: it is 0 at t=0, 0.5 at t=0.5 and 1 at t=1, but
    // 0.203 at t=0.25 against a linear 0.25. That is a smoothstep-ish S-curve -
    // on a 10.5 mm rise it departs from the straight line by 0.49 mm at the
    // quarter points. A surface built from those is not ruled, and the cut face
    // the user asked to be generated by straight lines would visibly bow.
    //
    // So when an axis has exactly two control points, blend along it LINEARLY.
    // This cannot change any square grid's behaviour: nx == ny == 2 was below the
    // old MinResolution of 3, so no existing sheet ever had a 2-count axis.
    auto blend = [](double p0, double p1, double p2, double p3, double t, int count) {
        return count == 2 ? p1 + (p2 - p1) * t : catmull_rom(p0, p1, p2, p3, t);
    };

    double col[4];
    for (int k = 0; k < 4; ++ k) {
        const int j = iv - 1 + k;
        col[k] = blend(z_at(iu - 1, j), z_at(iu, j), z_at(iu + 1, j), z_at(iu + 2, j), tu, m_nx);
    }
    return blend(col[0], col[1], col[2], col[3], tv, m_ny);
}

double CurvedCutSheet::evaluate_local(double x, double y) const
{
    const double u = 0.5 * (x / m_half_size_u + 1.0);
    const double v = 0.5 * (y / m_half_size_v + 1.0);
    return evaluate(u, v);
}

void CurvedCutSheet::set_grid(int nx, int ny)
{
    const int gx = std::clamp(nx, MinResolution, MaxResolution);
    const int gy = std::clamp(ny, MinResolution, MaxResolution);
    if (gx == m_nx && gy == m_ny)
        return;
    // Re-sample the current surface onto the new grid. Catmull-Rom interpolates
    // its control points, so going to a finer grid whose nodes include the old
    // ones (e.g. 3 -> 5 -> 9) reproduces the old values exactly at those nodes,
    // and coming back lands on them again. Each axis refines independently, so
    // 10 x 2 -> 19 x 3 is exact on both axes at once.
    std::vector<double> nz(size_t(gx) * size_t(gy), 0.0);
    if (!is_flat())
        for (int j = 0; j < gy; ++ j) {
            const double v = gy < 2 ? 0.5 : double(j) / double(gy - 1);
            for (int i = 0; i < gx; ++ i) {
                const double u = gx < 2 ? 0.5 : double(i) / double(gx - 1);
                nz[size_t(j) * size_t(gx) + size_t(i)] = evaluate(u, v);
            }
        }
    m_nx = gx;
    m_ny = gy;
    m_z  = std::move(nz);
    // The reference grid is sized to the OLD counts and is now unusable, and a
    // grid change is an edit of the surface anyway.
    publish_reference();
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void CurvedCutSheet::grab(const Vec2d& center_xy, double radius, double delta, bool falloff)
{
    if (radius <= 0.0 || delta == 0.0)
        return;
    for (int j = 0; j < m_ny; ++ j)
        for (int i = 0; i < m_nx; ++ i) {
            const double d = (control_xy(i, j) - center_xy).norm();
            if (d >= radius)
                continue;
            // The same quartic bump the Sculpt gizmo's brush uses.
            const double w = falloff ? double(Sculpt::falloff_weight(float(d), float(radius))) : 1.0;
            at(i, j) += delta * w;
        }
    publish_reference();
}

void CurvedCutSheet::smooth(double strength, const Vec2d* center_xy, double radius, bool falloff)
{
    // THE ny == 2 TRAP. The guard used to be "m_resolution < 3", i.e. "if either
    // axis is too short to have an interior point, do nothing". On a 10 x 2 sheet
    // that would switch smoothing off altogether, even though the u axis has
    // eight interior columns crying out for it. Only a grid that is short on BOTH
    // axes has nothing to smooth.
    if ((m_nx < 3 && m_ny < 3) || strength <= 0.0)
        return;
    strength = std::clamp(strength, 0.0, 1.0);

    const std::vector<double> src = m_z;
    auto z_at = [&src, this](int i, int j) {
        i = std::clamp(i, 0, m_nx - 1);
        j = std::clamp(j, 0, m_ny - 1);
        return src[size_t(j) * size_t(m_nx) + size_t(i)];
    };

    for (int j = 0; j < m_ny; ++ j)
        for (int i = 0; i < m_nx; ++ i) {
            double w = 1.0;
            if (center_xy != nullptr && radius > 0.0) {
                const double d = (control_xy(i, j) - *center_xy).norm();
                if (d >= radius)
                    continue;
                w = falloff ? double(Sculpt::falloff_weight(float(d), float(radius))) : 1.0;
            }
            // The 4-neighbour average with EDGE CLAMPING. On a grid with three
            // or more points on both axes this is exactly what it always was,
            // so a square sheet smooths bit for bit as before.
            //
            // A 2-POINT AXIS IS EXCLUDED FROM THE AVERAGE. The tempting reading
            // is that its neighbours clamp onto the point itself and contribute
            // nothing - but they do not: at ny == 2 the point in row 0 has row 1
            // as a REAL j+1 neighbour, so a plain 4-neighbour pass drags the two
            // rows toward each other and flattens the very separation that makes
            // the sheet ruled. (5 and -3 become 3 and -1 in one pass at full
            // strength.) Smoothing a ruled sheet must smooth ALONG the ruling,
            // never across it, so only axes that have an interior point take
            // part - which for a 10 x 2 grid means u alone.
            const bool   use_u = m_nx >= 3;
            const bool   use_v = m_ny >= 3;
            const double sum   = (use_u ? z_at(i - 1, j) + z_at(i + 1, j) : 0.0) +
                                 (use_v ? z_at(i, j - 1) + z_at(i, j + 1) : 0.0);
            const double avg   = sum / double(2 * (int(use_u) + int(use_v)));
            at(i, j) = src[size_t(j) * size_t(m_nx) + size_t(i)] * (1.0 - strength * w) + avg * (strength * w);
        }
    publish_reference();
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

indexed_triangle_set CurvedCutSheet::sample_sheet(int samples) const
{
    const int  n = std::max(samples, 2);
    indexed_triangle_set its;
    its.vertices.reserve(size_t(n) * size_t(n));
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * m_half_size_v;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * m_half_size_u;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(evaluate(u, v))));
        }
    }
    its.indices.reserve(size_t(n - 1) * size_t(n - 1) * 2);
    for (int j = 0; j + 1 < n; ++ j)
        for (int i = 0; i + 1 < n; ++ i) {
            const int a = j * n + i;
            const int b = j * n + i + 1;
            const int c = (j + 1) * n + i + 1;
            const int d = (j + 1) * n + i;
            // CCW seen from +Z: the sheet's own outward normal points up.
            its.indices.emplace_back(Vec3i32(a, b, c));
            its.indices.emplace_back(Vec3i32(a, c, d));
        }
    return its;
}

// ---------------------------------------------------------------------------
// The cutter solid
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Phase 2: fitting to the cross-section, and snapping to the surface
// ---------------------------------------------------------------------------

bool curved_cut_fit_extent(const indexed_triangle_set& mesh,
                           double&                     half_size_u,
                           double&                     half_size_v,
                           double                      margin_rel,
                           double                      margin_abs)
{
    if (mesh.empty())
        return false;

    // The cross-section's bounding box, gathered straight from the edges that
    // cross z == 0. No polygon assembly: the outline's BOUNDING BOX is all the
    // fit needs, and every point of the outline lies on such an edge, so the
    // box the crossings give is the box the outline has. That also sidesteps
    // every degenerate case a contour builder has to handle (an edge lying in
    // the plane, a vertex exactly on it, a non-manifold seam) - a crossing that
    // is counted twice, or a coplanar edge whose endpoints are both taken, only
    // ever contributes points that ARE on the section.
    double min_x =  std::numeric_limits<double>::max();
    double max_x = -std::numeric_limits<double>::max();
    double min_y =  std::numeric_limits<double>::max();
    double max_y = -std::numeric_limits<double>::max();
    bool   any   = false;

    auto take = [&](const Vec3d& p) {
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y());
        max_y = std::max(max_y, p.y());
        any   = true;
    };

    for (const Vec3i32& tri : mesh.indices)
        for (int e = 0; e < 3; ++ e) {
            const Vec3d a = mesh.vertices[tri(e)].cast<double>();
            const Vec3d b = mesh.vertices[tri((e + 1) % 3)].cast<double>();
            const double za = a.z(), zb = b.z();
            if (za == 0.0)
                take(a);
            if ((za < 0.0 && zb > 0.0) || (za > 0.0 && zb < 0.0)) {
                const double t = za / (za - zb);
                take(a + t * (b - a));
            }
        }

    if (!any)
        return false;

    // Degenerate in one axis (a plane grazing a flat face along a line) still
    // gives a usable fit once the margin is added, so only the "no crossing at
    // all" case above is a failure.
    const double ext_u = 0.5 * (max_x - min_x);
    const double ext_v = 0.5 * (max_y - min_y);
    half_size_u = std::max(ext_u + std::max(margin_rel * ext_u, margin_abs), 1e-6);
    half_size_v = std::max(ext_v + std::max(margin_rel * ext_v, margin_abs), 1e-6);
    return true;
}

bool curved_cut_fit_projection_extent(const indexed_triangle_set& mesh,
                                     double&                     half_size_u,
                                     double&                     half_size_v,
                                     double                      margin_rel,
                                     double                      margin_abs)
{
    if (mesh.empty())
        return false;

    // Every vertex, projected onto the plane's own (u,v) axes - which in the
    // plane frame is simply dropping z. No crossing test at all: the point of
    // this fit is that it does NOT depend on where the plane sits inside the
    // part, so a plane that misses the part entirely still gets a domain that
    // covers it (and the empty-side warning, not a collapsed sheet, is what
    // tells the user about the miss).
    double min_x =  std::numeric_limits<double>::max();
    double max_x = -std::numeric_limits<double>::max();
    double min_y =  std::numeric_limits<double>::max();
    double max_y = -std::numeric_limits<double>::max();
    for (const Vec3f& v : mesh.vertices) {
        min_x = std::min(min_x, double(v.x()));
        max_x = std::max(max_x, double(v.x()));
        min_y = std::min(min_y, double(v.y()));
        max_y = std::max(max_y, double(v.y()));
    }

    // The domain is centred on the plane's origin, which is where the sheet's
    // frame is, so the half extent has to reach the FURTHER side - taking half
    // the width would leave the part hanging out of an off-centre box.
    const double ext_u = std::max(std::abs(min_x), std::abs(max_x));
    const double ext_v = std::max(std::abs(min_y), std::abs(max_y));
    half_size_u = std::max(ext_u + std::max(margin_rel * ext_u, margin_abs), 1e-6);
    half_size_v = std::max(ext_v + std::max(margin_rel * ext_v, margin_abs), 1e-6);
    return true;
}

int curved_cut_default_resolution(double half_size_u, double half_size_v, double target_spacing, int min_res)
{
    if (target_spacing <= 0.0)
        return CurvedCutSheet::DefaultResolution;
    // Spacing is set by the LONGER axis: the grid is n x n over a rectangle, so
    // the long side is where the handles thin out first.
    const double span = 2.0 * std::max(half_size_u, half_size_v);
    // n points span (n - 1) cells.
    const int    n    = int(std::lround(span / target_spacing)) + 1;
    return std::clamp(n, std::max(min_res, CurvedCutSheet::MinResolution), CurvedCutSheet::MaxResolution);
}

void curved_cut_default_grid(double half_size_u, double half_size_v, int& nx, int& ny, double target_spacing, int min_res)
{
    if (target_spacing <= 0.0) {
        nx = ny = CurvedCutSheet::DefaultResolution;
        return;
    }
    // Each axis gets the count ITS OWN extent wants, so a long thin part stops
    // being forced to carry the long axis' density across the short one.
    // `min_res` floors both: the automatic fit never picks a ruled grid by
    // itself, since a 2-count axis is a deliberate choice about the shape of the
    // cut rather than a consequence of the part's proportions.
    const int lo = std::max(min_res, CurvedCutSheet::MinResolution);
    auto count = [&](double hs) {
        const int n = int(std::lround(2.0 * hs / target_spacing)) + 1;
        return std::clamp(n, lo, CurvedCutSheet::MaxResolution);
    };
    nx = count(half_size_u);
    ny = count(half_size_v);
}

void curved_cut_thickness_faces(double thickness, CutThicknessOffset offset, double& lo, double& hi)
{
    const double t = std::max(0.0, thickness);
    switch (offset) {
    case CutThicknessOffset::Above: lo = 0.0;       hi = t;        break;
    case CutThicknessOffset::Below: lo = -t;        hi = 0.0;      break;
    default:                        lo = -0.5 * t;  hi = 0.5 * t;  break;
    }
}

void curved_cut_empty_sides(const indexed_triangle_set& mesh,
                            const CurvedCutSheet&       sheet,
                            bool&                       upper_empty,
                            bool&                       lower_empty,
                            double                      thickness,
                            CutThicknessOffset          offset)
{
    double lo = 0.0, hi = 0.0;
    curved_cut_thickness_faces(thickness, offset, lo, hi);

    upper_empty = true;
    lower_empty = true;
    for (const Vec3f& v : mesh.vertices) {
        const double h = sheet.evaluate_local(double(v.x()), double(v.y()));
        const double d = double(v.z()) - h;
        if (d > hi)
            upper_empty = false;
        else if (d < lo)
            lower_empty = false;
        if (!upper_empty && !lower_empty)
            return;
    }
}

bool curved_cut_snap_distance(const indexed_triangle_set& mesh,
                              const Vec3d&                pos,
                              double&                     distance)
{
    if (mesh.empty())
        return false;

    // Walk the triangles and intersect the vertical line x = pos.x, y = pos.y
    // with each, keeping the hit whose |z - pos.z| is smallest. A line/triangle
    // test rather than a ray one: "nearest hit in EITHER direction" is what the
    // gesture wants (a handle floating above the part snaps down, one buried
    // inside snaps to whichever face is closer), and one pass gets both.
    bool   found = false;
    double best  = 0.0;

    for (const Vec3i32& tri : mesh.indices) {
        const Vec3d a = mesh.vertices[tri(0)].cast<double>();
        const Vec3d b = mesh.vertices[tri(1)].cast<double>();
        const Vec3d c = mesh.vertices[tri(2)].cast<double>();

        // Barycentric solve in the XY projection. A triangle seen edge-on from
        // +Z projects to zero area and is skipped: it can only add a hit that a
        // neighbouring, non-degenerate face already provides.
        const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
        if (std::abs(d) < 1e-12)
            continue;
        const double l0 = ((b.y() - c.y()) * (pos.x() - c.x()) + (c.x() - b.x()) * (pos.y() - c.y())) / d;
        const double l1 = ((c.y() - a.y()) * (pos.x() - c.x()) + (a.x() - c.x()) * (pos.y() - c.y())) / d;
        const double l2 = 1.0 - l0 - l1;
        const double eps = -1e-9;
        if (l0 < eps || l1 < eps || l2 < eps)
            continue;

        const double z    = l0 * a.z() + l1 * b.z() + l2 * c.z();
        const double sign = z - pos.z();
        if (!found || std::abs(sign) < std::abs(best)) {
            best  = sign;
            found = true;
        }
    }

    if (!found)
        return false;
    distance = best;
    return true;
}

indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples, double extent, double extent_v, double offset)
{
    const int n = std::max(samples, 2);

    // Floor well below anything the object reaches, and below the sheet itself.
    const double diag  = bbox.defined ? bbox.size().norm() : 100.0;
    const double slack = std::max(1.0, 0.1 * diag);
    const double floor_z = std::min(bbox.defined ? bbox.min.z() : -slack,
                                    -sheet.max_displacement() + std::min(0.0, offset)) - slack;

    indexed_triangle_set its;
    // The slab may be built WIDER than the sheet's own domain. Sampling by local
    // (x,y) through evaluate_local() - not by (u,v) - is what keeps the surface
    // itself fixed: over the sheet's domain the heights are unchanged, and beyond
    // it the clamped edge value is extruded straight outwards.
    const double hs   = extent   > 0.0 ? std::max(extent,   sheet.half_size_u()) : sheet.half_size_u();
    const double hs_v = extent_v > 0.0 ? std::max(extent_v, sheet.half_size_v())
                                       : (extent > 0.0 ? std::max(extent, sheet.half_size_v()) : sheet.half_size_v());

    // Top surface (the sheet) then the floor, both as n x n grids so the rim
    // stitches vertex-for-vertex and the slab comes out watertight.
    its.vertices.reserve(size_t(n) * size_t(n) * 2);
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * hs_v;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * hs;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(sheet.evaluate_local(x, y) + offset)));
        }
    }
    const int base = n * n;
    for (int j = 0; j < n; ++ j) {
        const double y = (2.0 * (double(j) / double(n - 1)) - 1.0) * hs_v;
        for (int i = 0; i < n; ++ i) {
            const double x = (2.0 * (double(i) / double(n - 1)) - 1.0) * hs;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(floor_z)));
        }
    }

    auto top = [n](int i, int j) { return j * n + i; };
    auto bot = [n, base](int i, int j) { return base + j * n + i; };

    // Top: outward normal +Z. Bottom: outward normal -Z (reversed winding).
    for (int j = 0; j + 1 < n; ++ j)
        for (int i = 0; i + 1 < n; ++ i) {
            its.indices.emplace_back(Vec3i32(top(i, j), top(i + 1, j), top(i + 1, j + 1)));
            its.indices.emplace_back(Vec3i32(top(i, j), top(i + 1, j + 1), top(i, j + 1)));
            its.indices.emplace_back(Vec3i32(bot(i, j), bot(i + 1, j + 1), bot(i + 1, j)));
            its.indices.emplace_back(Vec3i32(bot(i, j), bot(i, j + 1), bot(i + 1, j + 1)));
        }

    // The four side walls, vertical in local Z, stitched between the sheet's
    // boundary row and the matching floor row. This is the rim the research
    // spec calls out: without it the two sheets are two open surfaces, not a
    // solid, and the boolean has nothing to intersect.
    for (int i = 0; i + 1 < n; ++ i) {
        // y = -hs edge (outward -Y)
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i, 0), bot(i + 1, 0)));
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i + 1, 0), top(i + 1, 0)));
        // y = +hs edge (outward +Y)
        its.indices.emplace_back(Vec3i32(top(i, n - 1), top(i + 1, n - 1), bot(i + 1, n - 1)));
        its.indices.emplace_back(Vec3i32(top(i, n - 1), bot(i + 1, n - 1), bot(i, n - 1)));
    }
    for (int j = 0; j + 1 < n; ++ j) {
        // x = -hs edge (outward -X)
        its.indices.emplace_back(Vec3i32(top(0, j), top(0, j + 1), bot(0, j + 1)));
        its.indices.emplace_back(Vec3i32(top(0, j), bot(0, j + 1), bot(0, j)));
        // x = +hs edge (outward +X)
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j), bot(n - 1, j + 1)));
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j + 1), top(n - 1, j + 1)));
    }

    return its;
}

// Manifold first, mcut as the fallback - the same chain CutUtils' flexi_boolean
// and the Mesh Boolean gizmo use.
static bool curved_boolean(const TriangleMesh& a, const TriangleMesh& b, const std::string& op, TriangleMesh& out)
{
    std::vector<TriangleMesh> dst;
    bool ok = MeshBoolean::mfd::make_boolean(a, b, dst, op);
    if (!ok) {
        BOOST_LOG_TRIVIAL(warning) << "Curved cut: Manifold boolean " << op << " failed, falling back to mcut";
        dst.clear();
        try {
            MeshBoolean::mcut::make_boolean(a, b, dst, op);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Curved cut: mcut boolean " << op << " failed: " << ex.what();
            return false;
        }
    }
    if (dst.empty())
        return false;
    TriangleMesh merged = dst.front();
    for (size_t i = 1; i < dst.size(); ++ i)
        merged.merge(dst[i]);
    if (merged.empty())
        return false;
    out = std::move(merged);
    return true;
}

// ---------------------------------------------------------------------------
// The shared boolean core. See CurvedCut.hpp for what each of the four
// behaviours below defends against; all four were learned on the curved cut and
// all four are exactly as applicable to the drawn one, which is why this lives
// here instead of being written twice.
// ---------------------------------------------------------------------------

bool cut_with_solid(const indexed_triangle_set& mesh,
                    const indexed_triangle_set& cutter_lo,
                    const indexed_triangle_set& cutter_hi,
                    bool                        kerf,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    const char*                 log_tag)
{
    if (mesh.empty() || cutter_lo.empty())
        return false;
    if (kerf && cutter_hi.empty())
        return false;

    const std::string tag = log_tag != nullptr ? log_tag : "Cut";

    TriangleMesh object(mesh);

    // THE "ONLY ONE HALF SURVIVES" FIX, part 1: the object's winding.
    //
    // Both booleans decide "inside" from face orientation, so an object whose
    // triangles are wound inwards - a flipped-normal import, or a mesh a previous
    // repair left inside out - reports its COMPLEMENT as its interior. The
    // INTERSECTION then comes back empty (or as the whole part) while the A_NOT_B
    // still succeeds, and the caller sees exactly one half. Detect it from the
    // signed volume, which is negative for precisely this case, and flip the copy
    // handed to the boolean. its_volume() is one pass over the triangles and this
    // runs once per cut, so it costs nothing measurable.
    if (its_volume(object.its) < 0.f) {
        BOOST_LOG_TRIVIAL(warning) << tag << ": the input mesh is wound inwards (negative signed volume); "
                                             "flipping it before the boolean";
        for (Vec3i32& t : object.its.indices)
            std::swap(t(1), t(2));
    }

    const TriangleMesh  lo_mesh(cutter_lo);
    const TriangleMesh  hi_mesh      = kerf ? TriangleMesh(cutter_hi) : TriangleMesh();
    const TriangleMesh& upper_cutter = kerf ? hi_mesh : lo_mesh;

    // THE "ONLY ONE HALF SURVIVES" FIX, part 2: never let ONE failed boolean
    // cost the caller a half.
    //
    // Each side used to be its own all-or-nothing boolean, and a failure just
    // cleared that side and logged. Downstream, an empty mesh is indistinguishable
    // from "this half does not exist": add_cut_volume() returns early on an empty
    // mesh, the cloned ModelObject ends up with no volumes, and post_process()
    // drops it - so the user gets one part back from a two-part cut, with nothing
    // on screen to say why. That is the reported bug.
    //
    // So: run BOTH sides whenever either was asked for, and when exactly one came
    // back, recover the other from the complement - the missing half is
    // (object - kept), another boolean against a solid the first one already
    // proved workable. Only when the direct boolean AND the complement both fail
    // is a half really unavailable.
    // The complement recovery is only sound when the two halves partition the
    // object, which a kerf deliberately breaks (the band belongs to neither).
    // With a kerf a failed boolean stays failed rather than being "recovered" as
    // the other half plus the band.
    auto complement = [&object, kerf](const indexed_triangle_set& kept, indexed_triangle_set& out) -> bool {
        if (kept.empty() || kerf)
            return false;
        TriangleMesh rest;
        if (!curved_boolean(object, TriangleMesh(kept), "A_NOT_B", rest) || rest.its.empty())
            return false;
        out = rest.its;
        return true;
    };

    indexed_triangle_set lower_its, upper_its;
    bool have_lower = false, have_upper = false;
    {
        TriangleMesh out;
        if (curved_boolean(object, lo_mesh, "INTERSECTION", out) && !out.its.empty()) {
            lower_its  = std::move(out.its);
            have_lower = true;
        }
    }
    {
        TriangleMesh out;
        if (curved_boolean(object, upper_cutter, "A_NOT_B", out) && !out.its.empty()) {
            upper_its  = std::move(out.its);
            have_upper = true;
        }
    }

    if (!have_upper && have_lower) {
        BOOST_LOG_TRIVIAL(warning) << tag << ": the upper boolean gave nothing; recovering it as object - lower";
        have_upper = complement(lower_its, upper_its);
    }
    else if (!have_lower && have_upper) {
        BOOST_LOG_TRIVIAL(warning) << tag << ": the lower boolean gave nothing; recovering it as object - upper";
        have_lower = complement(upper_its, lower_its);
    }

    bool ok = true;
    if (lower != nullptr) {
        if (have_lower)
            *lower = std::move(lower_its);
        else {
            lower->clear();
            ok = false;
        }
    }
    if (upper != nullptr) {
        if (have_upper)
            *upper = std::move(upper_its);
        else {
            upper->clear();
            ok = false;
        }
    }
    return ok;
}

bool curved_cut_split(const indexed_triangle_set& mesh,
                      const CurvedCutSheet&       sheet,
                      indexed_triangle_set*       upper,
                      indexed_triangle_set*       lower,
                      int                         samples,
                      double                      thickness,
                      CutThicknessOffset          offset)
{
    if (mesh.empty())
        return false;

    // The bounding box only; the mesh itself goes to cut_with_solid(), which owns
    // the copy the boolean needs (and the winding flip that copy may want).
    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    // The slab must reach past the object on every side, or "below the sheet" is
    // only defined over part of it. WIDEN THE SLAB, never the sheet: set_half_size()
    // on the sheet would drag its control points outwards and stretch the surface,
    // so a small part under a large plane would get a differently shaped cut than
    // the one the gizmo drew (and, at a rotated plane where the object's footprint
    // in the cut frame is much larger than the sheet, a nearly flat one).
    // Phase 2: the sheet's domain is a rectangle, so the slab is widened PER
    // AXIS. Taking one square extent from the larger side would still cover the
    // object, but it would spend the slab's fixed sample budget on empty space
    // along the short axis and coarsen the surface where it actually cuts.
    double        extent   = sheet.half_size_u();
    double        extent_v = sheet.half_size_v();
    if (bbox.defined) {
        const double need_u = 1.05 * std::max(std::abs(bbox.min.x()), std::abs(bbox.max.x())) + 1.0;
        const double need_v = 1.05 * std::max(std::abs(bbox.min.y()), std::abs(bbox.max.y())) + 1.0;
        extent   = std::max(extent,   need_u);
        extent_v = std::max(extent_v, need_v);
    }

    const double t = std::max(0.0, thickness);
    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(t, offset, face_lo, face_hi);

    // PHASE 3: the kerf. With a thickness the two halves are cut by DIFFERENT
    // surfaces - the lower half at sheet - t/2, the upper at sheet + t/2 - so the
    // band between them is removed from both. At t == 0 the two offsets are both
    // zero and the two slabs are the same object, which is why the t == 0 path
    // builds ONE slab and runs exactly the boolean pair phase 2 ran: the
    // no-thickness output is not "close to" the old one, it is the old one.
    const bool kerf = t > 0.0;

    const indexed_triangle_set slab    = curved_cut_lower_slab(sheet, bbox, samples, extent, extent_v, face_lo);
    const indexed_triangle_set slab_hi = kerf ? curved_cut_lower_slab(sheet, bbox, samples, extent, extent_v, face_hi)
                                              : indexed_triangle_set();

    // Everything that was hard about the rest of this cut - the winding flip, the
    // both-sides-with-complement-recovery, the Manifold->mcut chain - lives in
    // cut_with_solid() now, shared verbatim with the drawn cut.
    return cut_with_solid(mesh, slab, kerf ? slab_hi : slab, kerf, upper, lower, "Curved cut");
}


// ---------------------------------------------------------------------------
// Phase 2 fixes: side visibility contract (see CurvedCut.hpp).
// ---------------------------------------------------------------------------

float curved_cut_side_alpha(CurvedCutSideVisibility v)
{
    switch (v) {
    case CurvedCutSideVisibility::Ghost:  return 0.25f;
    case CurvedCutSideVisibility::Hidden: return -1.f; // negative == discard, see gouraud.fs
    default:                              return 1.f;
    }
}

bool curved_cut_side_is_ghost(float alpha)
{
    return alpha > 0.f && alpha < 1.f;
}

bool curved_cut_has_ghost_side(float alpha_1, float alpha_2)
{
    return curved_cut_side_is_ghost(alpha_1) || curved_cut_side_is_ghost(alpha_2);
}

// ---------------------------------------------------------------------------
// Phase 4: connectors on a curved cut - the sheet's local frame
// ---------------------------------------------------------------------------

// Step for the finite differences that give the gradient and the curvature, in
// mm. The height field is a Catmull-Rom spline, so it is C1 and a central
// difference converges; 1e-3 mm is far below any feature a user can draw and far
// above the double-precision noise floor of evaluate_local().
static constexpr double SheetDiffStep = 1e-3;

static inline void sheet_gradient(const CurvedCutSheet& sheet, double x, double y, double& fx, double& fy)
{
    const double h = SheetDiffStep;
    fx = (sheet.evaluate_local(x + h, y) - sheet.evaluate_local(x - h, y)) / (2.0 * h);
    fy = (sheet.evaluate_local(x, y + h) - sheet.evaluate_local(x, y - h)) / (2.0 * h);
}

Vec3d curved_cut_sheet_normal(const CurvedCutSheet& sheet, double x, double y)
{
    // A height field z = f(x,y) has the (unnormalized) normal (-fx, -fy, 1).
    // The z component is 1 before normalization and stays positive after, so the
    // normal never flips - which is what lets a connector's "up" follow the
    // surface without ever turning the connector inside out.
    if (sheet.is_flat())
        return Vec3d::UnitZ();

    double fx = 0.0, fy = 0.0;
    sheet_gradient(sheet, x, y, fx, fy);
    Vec3d n(-fx, -fy, 1.0);
    const double len = n.norm();
    if (len < EPSILON)
        return Vec3d::UnitZ();
    return n / len;
}

Transform3d curved_cut_sheet_frame(const CurvedCutSheet& sheet, double x, double y, double z_angle)
{
    const Vec3d n = curved_cut_sheet_normal(sheet, x, y);

    // A FLAT sheet must give back the IDENTITY, not "a rotation that happens to
    // be numerically identity" - a curved-but-flat cut has to reproduce the
    // plane path's connector volumes bit for bit, and Eigen's Quaternion route
    // would not necessarily land on exactly 1/0/0/0.
    if (sheet.is_flat() || (n - Vec3d::UnitZ()).norm() < EPSILON) {
        if (std::abs(z_angle) < EPSILON)
            return Transform3d::Identity();
        return Transform3d(Eigen::AngleAxisd(z_angle, Vec3d::UnitZ()));
    }

    // Local X: the plane's own X projected onto the tangent plane. Continuous in
    // (x,y) and equal to +X on a flat patch, so a connector slid across the sheet
    // turns smoothly rather than snapping to some arbitrary perpendicular.
    Vec3d ex = Vec3d::UnitX() - n.dot(Vec3d::UnitX()) * n;
    if (ex.norm() < 1e-6) {
        // Nearly vertical surface: fall back to the plane's Y. A height field
        // cannot actually reach 90 degrees, so this only guards the numerics.
        ex = Vec3d::UnitY() - n.dot(Vec3d::UnitY()) * n;
    }
    ex.normalize();
    Vec3d ey = n.cross(ex);
    ey.normalize();

    Matrix3d m;
    m.col(0) = ex;
    m.col(1) = ey;
    m.col(2) = n;

    Transform3d frame(Transform3d::Identity());
    frame.linear() = m;
    if (std::abs(z_angle) >= EPSILON)
        frame = frame * Transform3d(Eigen::AngleAxisd(z_angle, Vec3d::UnitZ()));
    return frame;
}

double curved_cut_sheet_tilt_deg(const CurvedCutSheet& sheet, double x, double y)
{
    const Vec3d n = curved_cut_sheet_normal(sheet, x, y);
    const double c = std::clamp(n.z(), -1.0, 1.0);
    return std::acos(c) * 180.0 / PI;
}

double curved_cut_sheet_curvature_radius(const CurvedCutSheet& sheet, double x, double y)
{
    if (sheet.is_flat())
        return std::numeric_limits<double>::max();

    // Second derivatives by central differences, at a step large enough that the
    // second difference is not swamped by the noise in the first: the error of a
    // second central difference goes as eps/h^2, so h is taken bigger here than
    // for the gradient.
    const double h = 0.05;
    const double f  = sheet.evaluate_local(x, y);
    const double fxx = (sheet.evaluate_local(x + h, y) - 2.0 * f + sheet.evaluate_local(x - h, y)) / (h * h);
    const double fyy = (sheet.evaluate_local(x, y + h) - 2.0 * f + sheet.evaluate_local(x, y - h)) / (h * h);
    const double fxy = (sheet.evaluate_local(x + h, y + h) - sheet.evaluate_local(x + h, y - h)
                      - sheet.evaluate_local(x - h, y + h) + sheet.evaluate_local(x - h, y - h)) / (4.0 * h * h);

    double fx = 0.0, fy = 0.0;
    sheet_gradient(sheet, x, y, fx, fy);
    const double p = 1.0 + fx * fx + fy * fy;
    const double sp = std::sqrt(p);

    // Mean and Gaussian curvature of a Monge patch. The principal curvatures are
    // H +/- sqrt(H^2 - K); the LARGER |curvature| is the SMALLER radius, which is
    // the one that decides whether a straight-featured connector can sit flush.
    const double K = (fxx * fyy - fxy * fxy) / (p * p);
    const double H = ((1.0 + fy * fy) * fxx - 2.0 * fx * fy * fxy + (1.0 + fx * fx) * fyy) / (2.0 * p * sp);
    const double disc = std::max(0.0, H * H - K);
    const double root = std::sqrt(disc);
    const double k = std::max(std::abs(H + root), std::abs(H - root));
    if (k < 1e-9)
        return std::numeric_limits<double>::max();
    return 1.0 / k;
}

bool curved_cut_patch_is_flat_enough(const CurvedCutSheet& sheet, double x, double y, double extent)
{
    if (extent <= 0.0)
        return true;
    return curved_cut_sheet_curvature_radius(sheet, x, y) >= CurvedConnectorFlatPatchFactor * extent;
}

} // namespace Slic3r
