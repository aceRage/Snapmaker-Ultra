#ifndef slic3r_CurvedCut_hpp_
#define slic3r_CurvedCut_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "BoundingBox.hpp"

#include <algorithm>
#include <vector>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Curved cut, phase 1: a height field z = f(u,v) over the base cut plane.
//
// The cutting surface is NOT a free mesh. It is a single-valued function of the
// two in-plane coordinates, sampled from a coarse control grid. That is what
// makes it self-intersection-free by construction and what makes the zero
// displacement case degrade EXACTLY to the flat plane cut (see
// CurvedCutSheet::is_flat and Cut::perform_with_curved_sheet, which routes a
// flat sheet straight back into perform_with_plane so the output is identical).
//
// Coordinates: the sheet lives in the cut plane's own frame, the same frame
// cut_mesh() slices at z == 0. (u,v) run over [0,1]^2 and map linearly onto
// [-half_size_u, +half_size_u] x [-half_size_v, +half_size_v] in local X and Y;
// f is in millimetres along local Z.
// ---------------------------------------------------------------------------

class CurvedCutSheet
{
public:
    // Control grid resolution limits, PER AXIS. The maths works for any n >= 2;
    // an axis with only 2 points cannot bend along itself, it can only tilt -
    // which is the whole point of a RULED grid like 10 x 2, where each column is
    // one straight line the user controls from either end. (2 x 2 is therefore a
    // tilted plane, not a bend.)
    static const int MinResolution = 2;
    static const int MaxResolution = 15;
    static const int DefaultResolution = 5;
    // Dense sample resolution for the PREVIEW sheet. 64x64 is plenty to look
    // right and cheap to rebuild every drag tick.
    static const int DefaultSamples = 64;
    // Dense sample resolution for the CUT. The sampled sheet is piecewise
    // linear, so it sits below the true surface by a chord sag that falls as
    // 1/N^2: for a 8 mm dome over an 80 mm span that is 0.040 mm at 64x64 but
    // 0.0098 mm at 128x128. The cut face has to hold 0.02 mm, the preview does
    // not, so the cut samples finer than the preview does.
    static const int CutSamples = 128;

    CurvedCutSheet() { reset(DefaultResolution); }
    explicit CurvedCutSheet(int resolution) { reset(resolution); }
    CurvedCutSheet(int nx, int ny) { reset(nx, ny); }

    // Zero every displacement, optionally changing the grid counts. The one-int
    // form is the SQUARE form: it sets both counts, which is what every caller
    // from before non-square grids meant by it.
    void reset(int resolution = -1);
    void reset(int nx, int ny);

    // PHASE 5: the control grid is a RECTANGLE of counts, nx columns by ny rows,
    // so a ruled bend (10 x 2: every column a line, grabbable from either end)
    // is expressible. resolution() / set_resolution(int) are kept as the SQUARE
    // API - the same compatibility trick half_size() plays for the rectangular
    // domain - so every caller that only ever wanted "the grid is this dense"
    // still compiles and still means what it used to. resolution() reports the
    // LARGER count, matching half_size()'s "the bigger of the two" rule.
    int  resolution() const { return std::max(m_nx, m_ny); }
    int  nx() const { return m_nx; }
    int  ny() const { return m_ny; }
    // Change the control grid counts, re-sampling the CURRENT surface onto the
    // new grid so the shape is preserved as closely as the new grid can
    // represent it (exactly, when the new grid is a refinement). The one-int
    // form sets both counts.
    void set_resolution(int resolution) { set_grid(resolution, resolution); }
    void set_grid(int nx, int ny);

    // Half extent of the sheet's domain in the cut plane, in mm. The sheet must
    // cover the object's footprint under the cut plane.
    //
    // PHASE 2: the domain is a RECTANGLE, so u and v get their own half extents.
    // half_size() is kept as the square API (it reports the larger of the two,
    // and setting it makes the domain square again), so every caller that only
    // ever wanted "the sheet is this big" still compiles and still means what it
    // used to. Nothing about the height field changes: (u,v) still run over
    // [0,1]^2, they just map onto a rectangle now.
    double half_size() const { return std::max(m_half_size_u, m_half_size_v); }
    void   set_half_size(double hs) { set_half_size(hs, hs); }
    double half_size_u() const { return m_half_size_u; }
    double half_size_v() const { return m_half_size_v; }
    // Change the domain. `resample` re-samples the CURRENT surface onto the new
    // extent the way set_resolution() re-samples onto a new grid: each control
    // point takes the height the OLD surface had at the SAME local (x,y) in mm,
    // clamped at the old domain's border. The surface therefore stays put in the
    // cut plane while the rectangle around it grows or shrinks - which is what
    // "the shape survives a re-fit" has to mean, since a bend the user drew over
    // the part must not slide or scale when the plane is nudged.
    //
    // Without `resample` the control heights are left alone, so the surface is
    // STRETCHED onto the new rectangle - phase 1's set_half_size() behaviour,
    // kept for the callers (and tests) that want exactly that.
    //
    // PHASE 3: re-sampling is IDEMPOTENT. Re-sampling reads the surface through
    // Catmull-Rom and writes control values back, and that round trip is lossy
    // at a different grid phase - so re-sampling from the PREVIOUS re-sample
    // compounds the loss, and a plane drag (which produces a stream of re-fits)
    // used to walk the border away from where the user drew it. The sheet
    // therefore keeps a REFERENCE grid: the last surface an EDIT produced, with
    // the extent it was edited at. Every re-sample reads that reference, never
    // the previously re-sampled values, so N re-fits cost exactly what one costs
    // and returning to an earlier extent returns to that extent's values
    // bit-for-bit. Any edit (grab / smooth / reset / set_values / set_resolution)
    // republishes the reference.
    void set_half_size(double hs_u, double hs_v, bool resample = false);

    // Control point displacement along the plane normal, in mm. Row-major with
    // the ROW STRIDE = nx, so i indexes the column (u) and j the row (v).
    double  at(int i, int j) const { return m_z[size_t(j) * size_t(m_nx) + size_t(i)]; }
    double& at(int i, int j)       { return m_z[size_t(j) * size_t(m_nx) + size_t(i)]; }
    const std::vector<double>& values() const { return m_z; }
    void set_values(const std::vector<double>& z);

    // The reference extent the current control values were last EDITED at. Equal
    // to the live extent unless a re-sample has moved the domain since.
    double reference_half_size_u() const { return m_ref_half_size_u; }
    double reference_half_size_v() const { return m_ref_half_size_v; }
    // Adopt the current (re-sampled) values as the new reference, i.e. "this is
    // the shape now, forget where it came from". An edit does this implicitly.
    void   commit_reference();

    // --- frame flip --------------------------------------------------------
    // Carry the sheet through a 180-degree rotation of the CUT PLANE FRAME about
    // one of its own in-plane axes (the gizmo's "flip cut plane" / right-click
    // switch-sides gesture, which does m_rotation_m * rotation_transform(PI * X)).
    //
    // The frame's X survives, its Y and Z negate. A point that was at local
    // (x, y, z) is at (x, -y, -z) in the NEW frame, so for the world surface to
    // be unchanged the height field must satisfy
    //
    //     f_new(x, -y) = -f_old(x, y)
    //
    // which on the control grid is exactly "mirror the rows along v, negate every
    // value". flip_about_u() does that (a rotation about the frame's X: v and z
    // negate); flip_about_v() is the same for a rotation about the frame's Y
    // (u and z negate). Both are EXACT - no evaluation, no re-sampling, no loss -
    // and both are their own inverse.
    //
    // The control grid is symmetric in (u,v) about its centre, so mirroring the
    // indices lands exactly on grid points and the extent is unchanged; the
    // reference grid is republished, so a later re-fit re-samples the FLIPPED
    // surface rather than resurrecting the pre-flip one.
    //
    // PHASE 5, THE AXIS TRAP: flip_about_u() mirrors ALONG V, so its index runs
    // over NY; flip_about_v() mirrors along u, over NX. On a square grid the two
    // counts are equal and getting them the wrong way round is invisible - on a
    // 10 x 2 sheet it reads off the end of the grid. The tests pin both.
    void flip_about_u();
    void flip_about_v();

    // (u,v) in [0,1]^2 of control point (i,j). u runs over the nx COLUMNS, v over
    // the ny ROWS - they are different counts now, so they are different
    // functions. (control_xy() used to call control_u(j) for the v axis, which
    // was the core square assumption.)
    double control_u(int i) const { return m_nx < 2 ? 0.5 : double(i) / double(m_nx - 1); }
    double control_v(int j) const { return m_ny < 2 ? 0.5 : double(j) / double(m_ny - 1); }
    // Local (x,y) of control point (i,j), in the cut plane frame.
    Vec2d  control_xy(int i, int j) const;
    Vec3d  control_pos(int i, int j) const;

    // The height field. Catmull-Rom (not B-spline) on purpose: it INTERPOLATES
    // its control points, so a control point's own displacement is the surface
    // height there - which is what the sampling proof in the spec's test plan
    // measures, and what makes "all zero -> exactly flat" hold pointwise rather
    // than only approximately.
    double evaluate(double u, double v) const;
    // Same, from a point in the cut plane's local frame.
    double evaluate_local(double x, double y) const;

    // True when every control point is at zero, i.e. the surface is the plane
    // z == 0 and the cut must go through the plain flat path.
    bool is_flat() const;

    // Largest |displacement| over the control grid, in mm.
    double max_displacement() const;

    // --- editing -----------------------------------------------------------
    // Grab: move every control point within `radius` (measured in the plane,
    // in mm) by `delta` mm along the normal, weighted by the Sculpt gizmo's
    // falloff (MeshSculpt's falloff_weight) when `falloff` is set.
    void grab(const Vec2d& center_xy, double radius, double delta, bool falloff = true);
    // One Laplacian smoothing pass over the control grid. `strength` in [0,1]
    // blends between the original value and the 4-neighbour average; when
    // `radius` is positive only points within it (falloff-weighted) move.
    //
    // PHASE 5: a ruled grid smooths along the axis that HAS interior points, and
    // only that one. A 2-point axis is left out of the average entirely, so on a
    // 10 x 2 sheet the pass is a 1D smooth along u and the two rows keep their
    // separation - smoothing a ruled sheet must act along the ruling, not across
    // it. (Edge clamping alone would NOT give that: at ny == 2 row 0's j+1
    // neighbour is row 1, a real point, so a plain 4-neighbour average would pull
    // the rows together and flatten the ruling.) The old "either count < 3, do
    // nothing" guard would instead have turned smoothing off altogether here.
    void smooth(double strength = 0.5, const Vec2d* center_xy = nullptr, double radius = 0.0, bool falloff = true);

    // --- sampling ----------------------------------------------------------
    // Dense sheet as a triangle grid in the cut plane's frame, `samples` x
    // `samples` vertices. Used both for the preview and (thickened) for the cut.
    indexed_triangle_set sample_sheet(int samples = DefaultSamples) const;

private:
    // The reference grid + extent every re-sample reads from (see set_half_size).
    // Kept in step with m_z by commit_reference(), which every editing entry
    // point calls.
    void publish_reference();

    int                 m_nx{DefaultResolution};
    int                 m_ny{DefaultResolution};
    double              m_half_size_u{50.0};
    double              m_half_size_v{50.0};
    std::vector<double> m_z;
    double              m_ref_half_size_u{50.0};
    double              m_ref_half_size_v{50.0};
    std::vector<double> m_ref_z;
};

// ---------------------------------------------------------------------------
// Phase 2: fitting the sheet to the cut's own cross-section.
//
// Phase 1 sized the sheet from the object's bounding-box diagonal, so on
// anything that is not a cube most control points landed in empty space well
// outside the part and only a couple of them did anything. The fit below
// intersects the object with the cut plane and sizes the sheet to THAT outline
// instead, so the handles sit over the material being cut.
// ---------------------------------------------------------------------------

// Half extents (u,v) of `mesh`'s cross-section at z == 0 in the cut plane's own
// frame, plus a margin of max(`margin_rel` * extent, `margin_abs`) on each side.
// `mesh` must already be in the plane frame (the same frame cut_mesh() slices).
//
// Returns false when the plane misses the mesh entirely (no crossing edge), in
// which case the caller should keep the extent it has - an empty cross-section
// is not a reason to collapse the sheet to nothing.
bool curved_cut_fit_extent(const indexed_triangle_set& mesh,
                           double&                     half_size_u,
                           double&                     half_size_v,
                           double                      margin_rel = 0.15,
                           double                      margin_abs = 5.0);

// ---------------------------------------------------------------------------
// Phase 3: fitting the sheet to the WHOLE part, not to the cross-section.
//
// curved_cut_fit_extent() sizes the sheet to the plane's intersection with the
// object. That is the right place for the HANDLES but the wrong extent for the
// CUTTER: outside the sheet's own domain evaluate_local() clamps, so the slab
// extrudes the sheet's RIM height outwards, and a part that is wider above or
// below the plane than it is AT the plane gets sliced by that extruded rim
// rather than by the surface the user drew. Bend the sheet hard enough and the
// rim leaves the part entirely on one side, and that side comes back empty -
// the reported "it removes the smaller part".
//
// The fix is to fit the extent to the bounding box of the WHOLE mesh projected
// onto the plane's (u,v) axes, so no part of the object ever lies outside the
// sheet's own domain and the extruded rim never touches material. The handles
// stay usable because the projection is a superset of the cross-section, not a
// different place - see curved_cut_default_resolution() for keeping the spacing
// sane once the domain covers the whole part.
//
// Returns false only for an empty mesh.
bool curved_cut_fit_projection_extent(const indexed_triangle_set& mesh,
                                      double&                     half_size_u,
                                      double&                     half_size_v,
                                      double                      margin_rel = 0.15,
                                      double                      margin_abs = 5.0);

// The control-grid resolution to use for a domain of these half extents: enough
// points that the spacing lands near `target_spacing` mm, clamped into
// [min_res, MaxResolution]. Fitting to the whole projection makes the domain
// bigger than the cross-section fit did, so a fixed 5 x 5 would spread the
// handles too thin on a large part; this keeps them at a workable pitch.
int curved_cut_default_resolution(double half_size_u,
                                  double half_size_v,
                                  double target_spacing = 10.0,
                                  int    min_res        = 5);

// PHASE 5: the same answer PER AXIS. The single-answer form above is set by the
// LONGER axis, which on a long thin part spreads the short axis' handles far
// wider apart than the target spacing asks for; this gives each axis the count
// its own extent wants. `min_res` applies to both, so the auto fit never chooses
// a ruled grid on its own - a 10 x 2 is the user's decision, not the fit's.
void curved_cut_default_grid(double half_size_u,
                             double half_size_v,
                             int&   nx,
                             int&   ny,
                             double target_spacing = 10.0,
                             int    min_res        = 5);

// ---------------------------------------------------------------------------
// Phase 3: which side of the sheet is empty, cheaply.
//
// A pure sign test: for every vertex of `mesh` (in the plane frame), compare its
// local z against the sheet's height there. Any vertex strictly above makes the
// upper side non-empty, any vertex strictly below makes the lower one non-empty.
// This is what the gizmo warns from before the user commits to a cut - it costs
// one pass over the vertices, where the actual answer costs two booleans.
//
// It is a CONSERVATIVE test in the direction that matters: it can only claim a
// side is non-empty when a vertex is on that side, and a mesh with a vertex on
// one side always has material there. (The converse - a side with no vertex but
// with material, from a face crossing the sheet between its vertices - cannot
// happen for a closed mesh: the material on that side is bounded by faces, and
// those faces have vertices.)
//
// `thickness` is the kerf: with t > 0 the test is against the OFFSET faces, so a
// side counts as empty when the kerf has eaten everything that was on it.
// ---------------------------------------------------------------------------
// Phase 3: cut thickness ("kerf"), shared by the flat and the curved cut.
// ---------------------------------------------------------------------------

// The panel's range, in mm. 0 is "no kerf" and is the default; above 20 mm a
// "cut" is really "split into two parts with a big hole between them", which the
// user can get by cutting twice.
static constexpr double CutThicknessMin = 0.0;
static constexpr double CutThicknessMax = 20.0;

// Where the removed band sits relative to the cut surface.
enum class CutThicknessOffset {
    Centred,   // [surface - t/2, surface + t/2]   (the default)
    Above,     // [surface,       surface + t]     - the band is taken from the upper half
    Below      // [surface - t,   surface      ]   - ... from the lower half
};

// The two face offsets a thickness produces along the plane normal, in mm:
// `lo` is where the LOWER half's face sits, `hi` where the UPPER half's does.
// Always lo <= 0 <= hi and hi - lo == thickness (clamped at 0).
void curved_cut_thickness_faces(double thickness, CutThicknessOffset offset, double& lo, double& hi);

void curved_cut_empty_sides(const indexed_triangle_set& mesh,
                            const CurvedCutSheet&       sheet,
                            bool&                       upper_empty,
                            bool&                       lower_empty,
                            double                      thickness = 0.0,
                            CutThicknessOffset          offset    = CutThicknessOffset::Centred);

// Signed distance from `pos` (in the cut plane's frame) to the nearest surface
// of `mesh` (also in that frame) along the plane normal, i.e. along local Z.
// Both directions are tried and the NEARER hit wins; the sign is the local-Z
// offset of the hit from `pos`. This is the pure core of the gizmo's
// right-click "snap the handle onto the model" gesture.
//
// Returns false when the ray misses in both directions, in which case the
// caller must leave the control point where it is.
bool curved_cut_snap_distance(const indexed_triangle_set& mesh,
                              const Vec3d&                pos,
                              double&                     distance);

// Build the closed slab that everything below the sheet gets intersected with:
// the sheet, offset DOWN to a floor well below `bbox`, with a rim stitched
// round the boundary so the result is watertight. `bbox` is the object's
// bounding box in the cut plane's frame.
//
// The slab's side walls are vertical (along local Z), never along the sheet
// normal, so a steep sheet cannot make the slab self-intersect.
// `extent`, when positive, is the half extent the slab is BUILT at, which may be
// larger than the sheet's own domain so the slab reaches past the object. The
// sheet is never rescaled to fit: outside its domain evaluate_local() clamps and
// the boundary height is extruded outwards, so widening the slab moves no part of
// the surface that lies over the object.
// `extent` applies to BOTH axes; `extent_v`, when positive, overrides it for v so
// a rectangular sheet can be widened per axis. (A square `extent` still works and
// still means what it did in phase 1.)
// `offset`, in mm along local Z, raises or lowers the whole slab's TOP surface
// (the sheet) without touching the floor, which is what a cut thickness needs:
// the material to remove is the band between sheet - t/2 and sheet + t/2, i.e.
// the difference of two of these slabs. The rim and the floor are unchanged, so
// the result is watertight the same way.
indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples = CurvedCutSheet::CutSamples, double extent = -1.0, double extent_v = -1.0, double offset = 0.0);

// ---------------------------------------------------------------------------
// The shared boolean core, factored out of curved_cut_split() so the DRAW cut
// (DrawCut.hpp) runs the SAME chain rather than a second copy of it.
//
// It takes the two CUTTER SOLIDS and nothing else, so it knows nothing about
// height fields, strokes or planes:
//
//   lower = mesh INTERSECTION cutter_lo     ("the part inside the cutter")
//   upper = mesh A_NOT_B      cutter_hi     ("the part outside it")
//
// and carries all four of the behaviours the curved cut had to learn the hard
// way, which a second implementation would have had to learn again:
//
//   1. Manifold first, mcut as the fallback, multi-part results merged.
//   2. The NEGATIVE-SIGNED-VOLUME winding flip: a mesh wound inwards reports its
//      complement as its interior, so the INTERSECTION comes back empty and the
//      caller sees exactly one half.
//   3. BOTH sides always run, and a side that failed is recovered as the
//      complement (object A_NOT_B kept) - disabled when a kerf is in play, since
//      the two halves then deliberately do not partition the object.
//   4. The kerf's two-solid shape: cutter_lo != cutter_hi and `kerf` true.
//
// `kerf` must be true exactly when cutter_lo and cutter_hi are DIFFERENT solids.
// Pass the same solid twice with kerf == false for the plain two-boolean cut.
// Either output pointer may be null; returns false when a requested side has
// nothing (and clears it).
bool cut_with_solid(const indexed_triangle_set& mesh,
                    const indexed_triangle_set& cutter_lo,
                    const indexed_triangle_set& cutter_hi,
                    bool                        kerf,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    const char*                 log_tag = "Curved cut");

// Split `mesh` (already in the cut plane's frame) by the sheet. Returns false
// when both booleans failed. Either output pointer may be null.
// Manifold first, mcut as the fallback - the same chain the flexi joint cut and
// the Mesh Boolean gizmo use.
// `thickness`, in mm, is the KERF: a band of material centred on the sheet is
// removed, so the upper half keeps what is above sheet + t/2 and the lower half
// what is below sheet - t/2. thickness == 0 is the original two-boolean cut,
// bit-for-bit - the offset slabs are only built when t > 0.
bool curved_cut_split(const indexed_triangle_set& mesh,
                      const CurvedCutSheet&       sheet,
                      indexed_triangle_set*       upper,
                      indexed_triangle_set*       lower,
                      int                         samples   = CurvedCutSheet::CutSamples,
                      double                      thickness = 0.0,
                      CutThicknessOffset          offset    = CutThicknessOffset::Centred);

// ---------------------------------------------------------------------------
// Phase 4: connectors on a curved cut.
//
// A connector on a FLAT cut lives in the plane's own frame: one shared
// rotation_m for every connector, pos on z == 0. On a CURVED cut neither holds.
// The sheet is a surface, so a connector sits at (u,v) on it - at height
// f(u,v), NOT at zero - and it has to stand along the surface's own normal
// there, not along the plane's. Everything below is that frame, and it is all
// derived from the height field, so nothing here needs the sheet mesh.
//
// The frame is the SAME SHAPE the flat path already uses (a rotation about the
// connector's own origin), which is why the whole cut path downstream is
// unchanged: a connector volume reaches Cut carrying
// translation_transform(pos) * rotation_m, and phase 4 only changes what those
// two are. On a flat sheet the normal is +Z and the frame is the identity, so
// a curved-but-flat cut produces the same connector volumes, bit for bit.
// ---------------------------------------------------------------------------

// Unit surface normal of the sheet at local (x,y) in the cut plane's frame,
// from the height-field gradient: n = normalize(-df/dx, -df/dy, 1). Always has
// a positive z component (a height field cannot overhang), so "up" never flips.
// Outside the domain evaluate_local() clamps and the gradient goes to zero, i.e.
// the normal is +Z - the same thing the extruded rim does to the cut.
Vec3d curved_cut_sheet_normal(const CurvedCutSheet& sheet, double x, double y);

// The point on the sheet above local (x,y): (x, y, f(x,y)).
inline Vec3d curved_cut_sheet_point(const CurvedCutSheet& sheet, double x, double y)
{
    return Vec3d(x, y, sheet.evaluate_local(x, y));
}

// The connector's LOCAL FRAME at local (x,y), as a rotation in the cut plane's
// frame: local Z is the sheet normal there, and local X is the plane's own X
// projected onto the tangent plane and then turned by `z_angle` radians about
// the normal. That projection is what makes the frame CONTINUOUS over the sheet
// and equal to the identity on a flat one - picking any old perpendicular would
// spin the connector as it slides.
//
// Degenerate case: when the surface is so steep that the plane's X is nearly
// parallel to the normal, the plane's Y is projected instead. A height field
// cannot reach 90 degrees, so this only guards the numerics.
Transform3d curved_cut_sheet_frame(const CurvedCutSheet& sheet, double x, double y, double z_angle = 0.0);

// Angle between the sheet normal at (x,y) and the plane normal (+Z), in degrees.
// The gizmo warns above CurvedConnectorTiltWarnDeg: a connector standing that
// far off the build direction prints at an angle and may need supports.
double curved_cut_sheet_tilt_deg(const CurvedCutSheet& sheet, double x, double y);

// Above this tilt (degrees) the gizmo warns that the connector prints at an angle.
static constexpr double CurvedConnectorTiltWarnDeg = 60.0;

// The smaller principal radius of curvature of the sheet at local (x,y), in mm,
// from the second derivatives of the height field. Returns a large value
// (std::numeric_limits<double>::max()) where the surface is locally flat.
//
// A Hinge or a Thread needs a locally FLAT patch: its knuckle run / its pitch
// line is a straight feature generated as if for a plane, so on a surface whose
// radius of curvature is comparable to the connector's own extent the body will
// not sit flush. The gizmo warns (it does not block) below
// CurvedConnectorFlatPatchFactor times the connector's extent.
double curved_cut_sheet_curvature_radius(const CurvedCutSheet& sheet, double x, double y);

// A locally flat patch means radius >= this many times the connector's extent.
static constexpr double CurvedConnectorFlatPatchFactor = 3.0;

// True when a connector of this extent (its largest in-plane half size, mm) at
// local (x,y) sits on a patch flat enough for a straight-featured kind (Hinge,
// Thread) to mate. Purely advisory - the gizmo warns, nothing refuses.
bool curved_cut_patch_is_flat_enough(const CurvedCutSheet& sheet, double x, double y, double extent);

// ---------------------------------------------------------------------------
// Phase 2 fixes: side visibility, as a PURE contract.
//
// The gizmo lets each half of the preview be Visible, Ghost or Hidden, and the
// state reaches the fragment shader as one float per side. Nobody can look at a
// headless build's framebuffer, so the mapping and the draw-order predicate that
// depends on it live here, out of the GUI, where a test can pin them:
//
//   Visible -> 1.0   solid, drawn in the opaque pass with depth writes ON
//   Ghost   -> 0.25  blended, drawn in a SECOND pass with depth writes OFF
//   Hidden  -> < 0   discarded in the shader (a zero alpha would still write
//                    depth and go on occluding, which is the opposite of hiding)
//
// The two-pass split is the fix for "Ghost showed only the cut face": a single
// pass with depth writes off for the WHOLE volume left the solid half with no
// depth buffer either, so its own back faces blended over its front faces and
// all that survived was the cap. See GLVolumeCollection::render.
// ---------------------------------------------------------------------------

enum class CurvedCutSideVisibility { Visible, Ghost, Hidden };

// The per-side alpha uniform (color_clip_side_alpha_1 / _2 in gouraud.fs).
float curved_cut_side_alpha(CurvedCutSideVisibility v);

// True when `alpha` is a GHOST alpha: strictly between fully transparent and
// fully solid. Hidden (negative) is not a ghost - it is a discard - and Visible
// (1.0) is not one either.
bool curved_cut_side_is_ghost(float alpha);

// True when either side is ghosted, i.e. the draw needs the extra blended pass.
bool curved_cut_has_ghost_side(float alpha_1, float alpha_2);

} // namespace Slic3r

#endif /* slic3r_CurvedCut_hpp_ */
