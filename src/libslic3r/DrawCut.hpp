#ifndef slic3r_DrawCut_hpp_
#define slic3r_DrawCut_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "BoundingBox.hpp"
#include "CurvedCut.hpp"

#include <vector>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Draw cut, phase 1: the cut surface is a RULED STRIP swept along a stroke the
// user draws on the model's surface.
//
// This is a PEER of the curved sheet, not a variant of it. The sheet is a height
// field z = f(u,v) over the whole cut plane - single-valued by construction,
// which is what makes it self-intersection-free and what makes a zero sheet the
// flat cut exactly. A drawn stroke is a CURVE, and the surface it generates is
// swept along that curve: it is not single-valued over the plane, so it cannot
// be a CurvedCutSheet. Rasterising a stroke into a height field would lose the
// undercuts and draft angles the feature exists for.
//
// What IS shared is everything downstream: draw_cut_split() builds a closed
// cutter solid and hands it to cut_with_solid() (CurvedCut.hpp), the same
// Manifold->mcut chain with the same winding flip, the same
// both-sides-with-complement-recovery and the same two-solid kerf that the
// curved cut uses. Nothing about the boolean is written twice.
//
// Coordinates: the stroke lives in the CUT PLANE's own frame, the same frame
// cut_mesh() slices at z == 0 and the same frame CurvedCutSheet lives in. That
// is what lets the stroke survive a plane nudge the way the sheet does.
//
// PHASE 1 SCOPE: capture, the as-is directions (Surface normal with no angle,
// View, Axis X/Y/Z), Extension, Depth (through-all by default), the split, and
// the gates. No draft angle, no connectors on the drawn surface, no line editing
// past clear/undo. Phase 2 adds the angle (the binormal rotation with parallel
// transport), the fold clamp, connectors and control points.
// ---------------------------------------------------------------------------

// One captured point of a stroke: where the ray hit the model, the surface
// normal there, and which facet it was. All in the cut plane's frame; the normal
// is a unit vector in that frame too.
struct DrawCutSample
{
    Vec3d  pos{ Vec3d::Zero() };
    Vec3d  normal{ Vec3d::UnitZ() };
    size_t facet{ 0 };
};

// Which way the cut surface reaches INTO the part.
enum class DrawCutDirection {
    // Along the inward surface normal at each sample: the cut goes straight in,
    // perpendicular to the face the stroke was drawn on, so it follows the shape
    // the user drew on. Per-sample, so the surface is genuinely swept.
    SurfaceNormal,
    // Along a single constant direction, the same at every sample - an "as-is"
    // extrusion, i.e. a prism through the stroke. View takes the camera's own
    // forward direction (the gizmo supplies it, already in the plane frame);
    // AxisX/Y/Z take a plane-frame axis.
    View,
    AxisX,
    AxisY,
    AxisZ
};

struct DrawCutParams
{
    DrawCutDirection direction{ DrawCutDirection::SurfaceNormal };
    // The constant direction for DrawCutDirection::View, in the CUT PLANE's
    // frame, pointing INTO the part (i.e. away from the camera). Ignored for
    // every other direction. Need not be normalised.
    Vec3d            view_dir{ -Vec3d::UnitZ() };
    // How far the surface reaches PAST the stroke, in mm - measured ALONG the cut
    // direction, not sideways (see draw_cut_cutter_solid for why sideways would be
    // a draft rather than an extension). It does two jobs: it lifts the surface's
    // rim clear of the face the stroke was drawn on, so a stroke in a concavity is
    // not left with its rim buried in material, and for an OPEN stroke it is also
    // how far past each END the surface reaches along the tangent - which is what
    // lets the cut get past the silhouette so the boolean separates the part.
    double           extension{ 5.0 };
    // PHASE 2. The DRAFT ANGLE, in DEGREES, signed. It tilts the sweep direction
    // away from the local surface normal toward the stroke's OUTWARD binormal,
    // per sample:
    //
    //   d_i = cos(theta) * (-n_i) + sin(theta) * b_i
    //
    // POSITIVE FLARES OUTWARD - the ruling leans away from the loop's interior as
    // it goes in, so a closed stroke's plug widens with depth and the surrounding
    // shell opens out like a moulding draft. NEGATIVE UNDERCUTS - the plug narrows
    // with depth, so it is a dovetail that cannot be pulled straight out.
    //
    // The sign is tied to the same outward binormal DrawCutStroke::binormal()
    // defines, which is winding-independent, so a loop drawn clockwise and one
    // drawn counter-clockwise draft the same way.
    //
    // Only DrawCutDirection::SurfaceNormal uses it. The constant-direction modes
    // (View, Axis X/Y/Z) are by definition ONE direction at every sample - that is
    // what "as-is extrusion" means - and a per-sample tilt is exactly what they are
    // not, so draw_cut_inward_dir() ignores it there and the panel greys it out.
    double           angle_deg{ 0.0 };
    // How far it reaches IN. `through_all` ignores `depth` and uses a reach
    // derived from the object's bounding box, large enough to exit any side.
    bool             through_all{ true };
    double           depth{ 10.0 };
    // Kerf, in mm, and where the removed band sits. Shared with the flat and the
    // curved cut (CurvedCut.hpp).
    double           thickness{ 0.0 };
    CutThicknessOffset thickness_offset{ CutThicknessOffset::Centred };
};

// Why a stroke cannot be cut with. Kept as an enum rather than a bool so the
// panel can say WHICH thing is wrong - the spec asks for a clear error, and
// "the line crosses itself" and "draw a longer line" want different words.
enum class DrawCutError {
    None = 0,
    // Fewer than MinSamples after resampling, or an arc length under MinLength.
    TooShort,
    // A closed stroke that crosses itself has no well-defined interior, and an
    // open one that does has no well-defined upper side either. Phase 2 could
    // split it into sub-loops; phase 1 refuses.
    SelfCrossing,
    // The stroke jumped across empty space: the gap between consecutive HITS was
    // wider than a few sample spacings, so bridging it would have run a chord
    // through air. finish() repairs that by keeping the LONGEST contiguous run,
    // and when that run is usable there is no error at all - a line that crossed a
    // hole is meant to work. This is reported when the kept run is itself too
    // short, because then the reason the user needs is "your line left the model"
    // rather than "draw a longer line": their line WAS long enough.
    LeavesMesh,
    // The generated cutter, or one of the booleans, produced nothing usable.
    CutterDegenerate,
    // The cut ran but one side came back with no material.
    EmptySide
};

// Human-readable, untranslated. The gizmo wraps these in _L(); the tests read
// the enum.
const char* draw_cut_error_message(DrawCutError err);

// ---------------------------------------------------------------------------
// The stroke.
// ---------------------------------------------------------------------------

class DrawCutStroke
{
public:
    // Default resample spacing, in mm of arc length.
    static constexpr double DefaultSpacing = 1.0;
    // A stroke needs at least this many samples and this much arc length to
    // generate a surface at all. Four samples is three spans - the minimum that
    // can have a tangent at an interior point.
    static constexpr int    MinSamples = 4;
    static constexpr double MinLength  = 2.0;
    // Smoothing: the panel's 0..1 maps onto 0..MaxSmoothPasses windowed-average
    // passes over the positions and the normals. A raw stroke picks up every
    // triangle normal it crossed, and an unsmoothed normal field visibly kinks
    // the ruled surface.
    static constexpr int    MaxSmoothPasses = 10;

    DrawCutStroke() = default;

    // --- capture ----------------------------------------------------------
    // Append one raw sample. Capture appends in chronological order and does no
    // filtering: finish() is where resampling, smoothing and the open/closed
    // decision happen.
    void append(const Vec3d& pos, const Vec3d& normal, size_t facet = 0);
    void clear();
    bool empty() const { return m_samples.empty(); }
    size_t size() const { return m_samples.size(); }
    const std::vector<DrawCutSample>& samples() const { return m_samples; }

    // Resample to `spacing` arc length, smooth by `smoothing` in [0,1], then
    // decide open vs closed. Returns the error state, which is also what
    // error() reports afterwards. `force_closed` snaps the ends together even
    // when they did not meet within the tolerance.
    //
    // Idempotent in the sense that matters: calling it twice with the same
    // arguments on the same raw samples gives the same result, because it always
    // starts from the RAW samples rather than from the last finished ones.
    DrawCutError finish(double spacing = DefaultSpacing, double smoothing = 0.2, bool force_closed = false);

    // --- the finished stroke ----------------------------------------------
    // The resampled, smoothed samples finish() produced. Empty until finish()
    // has run and succeeded.
    const std::vector<DrawCutSample>& path() const { return m_path; }
    // PHASE 2. Replace the finished path IN PLACE, keeping the open/closed
    // decision, and recompute the binormals from it.
    //
    // This exists for exactly one caller: the gizmo's re-projection of the smoothed
    // path back onto the mesh (phase 1 deviation #7). That has to happen AFTER
    // finish() - smoothing is what moves the samples off the surface - and it must
    // not re-resample, because the path already is resampled and running the
    // resampler over its own output would walk the samples a little further every
    // time a slider moved. The binormals DO have to be recomputed, because they are
    // built from the positions and the normals this replaces.
    //
    // Refuses a path of a different length, which would invalidate the closed
    // decision finish() made.
    void set_path(const std::vector<DrawCutSample>& path);
    bool         is_closed() const { return m_closed; }
    DrawCutError error() const { return m_error; }
    bool         valid() const { return m_error == DrawCutError::None && m_path.size() >= size_t(MinSamples); }
    // Arc length of the finished path, in mm. For a closed stroke the closing
    // span counts.
    double       length() const;
    // Centroid of the finished path's positions.
    Vec3d        centroid() const;

    // Unit tangent at path sample i, central-differenced (and wrapped for a
    // closed stroke). Zero-length only for a degenerate path.
    Vec3d        tangent(size_t i) const;
    // The OUTWARD binormal at path sample i: normalize(t x n), with its sign
    // fixed so it points AWAY from the loop's interior for a closed stroke
    // (b . (p - centroid) > 0). Winding therefore decides, so a loop drawn
    // clockwise and one drawn counter-clockwise give the same plug and the user
    // does not have to care which way they dragged.
    //
    // For an open stroke there is no interior, so the sign is carried along the
    // stroke from the first sample (the branch that keeps b_i . b_{i-1} > 0) -
    // a pointwise t x n flips across an inflection and would tear the strip.
    Vec3d        binormal(size_t i) const;

private:
    void compute_binormals();

    std::vector<DrawCutSample> m_samples;  // raw, as captured
    std::vector<DrawCutSample> m_path;     // resampled + smoothed
    std::vector<Vec3d>         m_binormal; // per path sample, sign-continuous
    bool                       m_closed{ false };
    DrawCutError               m_error{ DrawCutError::TooShort };
};

// ---------------------------------------------------------------------------
// The three gaps the research spec names. Only the resampler is needed in phase
// 1 (a dragged stroke, not click-by-click, and no Catmull-Rom over a space
// curve until phase 2's control points).
// ---------------------------------------------------------------------------

// Resample a 3D polyline with per-sample normals to a fixed arc length.
//
// There is NO 3D resampler in this codebase to reuse: Polyline's
// equally_spaced_points / simplify are 2D integer Points, and Polyline3 is a stub
// declaring only lines(). So: walk the arc length, lerp position and normal,
// renormalise the normal.
//
// It does NOT inherit equally_spaced_points' wart of emitting the first point but
// not reliably the last: the final input sample is always appended (unless it
// lands within half a spacing of the previous output, where appending it would
// produce a short span the tangent maths would then trip on).
//
// `closed` resamples the closing span too and leaves the ring OPEN in the output
// (the last sample is not a duplicate of the first) - which is the representation
// the strip builder wants, since it wraps.
std::vector<DrawCutSample> draw_cut_resample(const std::vector<DrawCutSample>& in, double spacing, bool closed = false);

// N windowed-average passes over positions and normals, `strength` in [0,1]
// blending each sample toward the average of its two neighbours. Endpoints are
// held for an open stroke and wrapped for a closed one. Normals are renormalised
// after every pass.
//
// Nothing here re-projects onto the mesh: that needs a raycaster, which lives in
// the GUI. The gizmo re-projects after smoothing (MeshRaycaster::get_closest_point);
// headless, the samples stay where the average puts them, which for the small
// displacements a couple of passes produce is within the chord sag of the surface
// anyway.
void draw_cut_smooth(std::vector<DrawCutSample>& path, int passes, bool closed, double strength = 0.5);

// The passes a panel `smoothing` in [0,1] asks for.
int draw_cut_smooth_passes(double smoothing);

// Closing tolerance: a stroke is CLOSED when its last sample is within this of
// its first. max(3 * spacing, 2 mm) - three spacings so a hand-drawn loop that
// nearly met still counts, and a 2 mm floor so a finely resampled stroke does
// not need pixel accuracy.
double draw_cut_closing_tolerance(double spacing);

// ---------------------------------------------------------------------------
// The cut direction, and the draft angle that tilts it. PHASE 2.
// ---------------------------------------------------------------------------

// The panel's limit on the draft angle, in degrees, and the limit the geometry
// itself imposes. 60 degrees is already a very deep undercut / flare; past about
// 80 the ruling is so close to tangent to the surface that the strip grazes the
// face it was drawn on for its whole length and the boolean has nothing clean to
// work with.
static constexpr double DrawCutMaxAngleDeg = 60.0;

// The unit ray at path sample `i`, pointing INTO the part - the ruling direction
// of the swept strip, and the one place the draft angle is applied.
//
// For the constant directions it is that direction, at every sample. For Surface
// normal it is the inward normal rotated toward the OUTWARD binormal by
// params.angle_deg:
//
//   d_i = cos(theta) * (-n_i) + sin(theta) * b_i
//
// which is a rotation in the plane the two span, so d stays unit and the ruling
// stays a straight line. theta == 0 gives -n_i exactly, which is phase 1's
// behaviour bit for bit.
//
// Exposed (it was file-static in phase 1) because the fold guard, the surface
// frame and the tests all have to ask the SAME question the cutter builder asks.
Vec3d draw_cut_inward_dir(const DrawCutStroke& stroke, const DrawCutParams& params, size_t i);

// True when a CLOSED stroke's binormal field does not close up on itself - the
// frame comes back flipped after going round the loop, so there is no consistent
// "outward" side and a draft angle would flare one way on one part of the loop
// and the other way on the rest (a Moebius path on the surface).
//
// compute_binormals() already orients a closed loop's field from the centroid
// rather than by transport, so the field itself never tears; what this detects is
// the case where that orientation is fighting the surface - adjacent binormals
// that disagree in sign. The caller falls back to angle 0 and warns.
bool draw_cut_frame_holonomy_flips(const DrawCutStroke& stroke);

// ---------------------------------------------------------------------------
// The cutter solid.
// ---------------------------------------------------------------------------

// Build the closed cutter solid for `stroke` under `params`, in the cut plane's
// frame. `bbox` is the object's bounding box in that frame, used for the
// through-all reach. `face_offset`, in mm, displaces the whole surface along its
// own local normal (t x d, the strip's own normal) - which is how the kerf's two
// solids are built: offsetting along a global Z would measure the band wrong
// wherever the strip is not vertical.
//
// The strip is triangulated as a quad grid between two rails, both of which lie
// on the ruling line through p_i along the cut direction d_i - the ruling is
// STRAIGHT at phase 1's angle of zero, so nothing here moves sideways:
//
//   out_i = p_i - E * d_i    (back OUT along the cut direction, so the surface
//                             starts clear of the face the stroke was drawn on -
//                             otherwise a stroke in a concavity has its rim
//                             buried in material and the boolean leaves a skin)
//   in_i  = p_i + D * d_i    (in by Depth, or through the bbox)
//
// E must NOT push the outward rail sideways along the binormal, tempting as the
// phrase "how far the surface reaches past the stroke" makes it: that turns every
// closed cut into a DRAFT (the strip then runs from radius r + E down to radius r,
// so a 12 mm circle at E = 5 cuts a truncated cone of 1.65x the intended volume
// instead of a cylinder). Phase 2's draft angle tilts d ITSELF toward the
// binormal, which is where the binormal earns its keep. For an OPEN stroke,
// "reaching past" is the TANGENT extension at the two ends, below.
//
// and then CLOSED, because a boolean needs a solid and a strip is an open
// surface:
//
//   closed stroke: the band is a tube. Cap the inner ring and the outer ring, so
//                  the solid's interior is "everything the plug occupies".
//   open stroke:   extend both ends by Extension along the stroke tangent, then
//                  close the two end quads and both caps. The extended ends are
//                  what let the surface reach past the silhouette.
//
// Returns an empty set when the stroke is not valid().
indexed_triangle_set draw_cut_cutter_solid(const DrawCutStroke& stroke,
                                           const DrawCutParams& params,
                                           const BoundingBoxf3& bbox,
                                           double               face_offset = 0.0);

// ---------------------------------------------------------------------------
// THE DRAWN SURFACE AS A SURFACE. PHASE 2, and what connectors stand on.
//
// The curved cut's connectors ride on curved_cut_sheet_frame(): a rotation built
// from the sheet's own local normal, composed after the plane's m_rotation_m, so
// nothing in the connector path itself has to know about the sheet. The drawn
// surface needs the same three functions against the RULED STRIP instead of the
// height field.
//
// The strip's parameters are (s, w):
//   s - arc length along the stroke, in mm from the first path sample. For a
//       closed stroke it wraps at length().
//   w - the ruled parameter, in mm along the ruling from the stroke itself:
//       w == 0 is on the stroke, w > 0 is INTO the part, w < 0 is out of it. The
//       strip the cutter builds spans w in [-extension, +depth].
// ---------------------------------------------------------------------------

// The point of the ruled surface at (s, w), in the cut plane's frame.
Vec3d draw_cut_surface_point(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// The strip's OWN unit normal at (s, w): normalize(t x d), the direction the two
// halves separate along. Sign-fixed the same way the cutter's `sweep` is, so it
// is continuous along the stroke and points consistently to one side.
//
// It is w-independent for a straight ruling, which is what the ruling is here -
// the parameter is taken so callers do not have to know that, and so a future
// twisted ruling would not change the signature.
Vec3d draw_cut_surface_normal(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// A right-handed frame ON the drawn surface at (s, w), expressed in the cut
// plane's frame: local Z is the surface normal above, local X is the plane's own
// X projected onto the tangent plane (falling back to the plane's Y where that
// degenerates - the same construction and the same guard curved_cut_sheet_frame()
// uses), and `z_angle` spins the frame about its own Z the way a connector's
// Rotation does.
//
// A connector placed with this frame stands PERPENDICULAR TO THE CUT SURFACE, so
// its hole in one half and its plug in the other are coaxial by construction and
// survive the split - and, because both halves are cut by the same strip, they
// survive the kerf too (the kerf moves both faces along this same normal).
Transform3d draw_cut_surface_frame(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double z_angle = 0.0);

// The (s, w) of the point on the drawn surface CLOSEST to `p` (in the plane's
// frame), and the distance to it. This is the inverse of draw_cut_surface_point()
// and it is what turns a raycast hit on the cutter shell into surface parameters
// the frame can be built from.
//
// Returns false only for a stroke that is not valid().
bool draw_cut_surface_project(const DrawCutStroke& stroke, const DrawCutParams& params,
                              const Vec3d& p, double& s, double& w, double* distance = nullptr);

// True when (s, w) lies within the strip's own domain - the Draw analogue of the
// curved cut's (u,v)-in-contour test. `w` must be inside [-extension, +depth] with
// `margin` mm of room on each side (the connector's own radius, so a connector is
// not hung off the rim), and for an OPEN stroke `s` must likewise be `margin`
// clear of both ends. A closed stroke wraps, so `s` is never out of range there.
bool draw_cut_surface_contains(const DrawCutStroke& stroke, const DrawCutParams& params,
                               double s, double w, double margin, double depth_reach);

// The curvature radius of the strip ACROSS the rules at (s, w), in mm - i.e. how
// sharply the surface bends as you walk ALONG the stroke. Along the rules the
// strip is developable (a straight ruling has zero curvature that way), which is
// exactly why a straight-featured connector sits flatter here than on a dome:
// only one of the two directions can be curved at all.
//
// Infinity (a huge number) where the strip is locally flat.
double draw_cut_surface_curvature_radius(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// The analogue of curved_cut_patch_is_flat_enough(): the patch under a connector
// of half-extent `extent` is flat enough when the cross-rule curvature radius is
// at least CurvedConnectorFlatPatchFactor times that extent. Same constant, same
// meaning, so the panel's wording does not have to change between the two modes.
bool draw_cut_patch_is_flat_enough(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double extent);

// The tilt of the drawn surface's normal away from the cut plane's own +Z, in
// degrees - the analogue of curved_cut_sheet_tilt_deg(). A connector standing on
// a steeply tilted patch prints at that angle, which is what
// CurvedConnectorTiltWarnDeg warns about.
double draw_cut_surface_tilt_deg(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// True when the stroke's projection onto its own best-fit plane crosses itself.
// A self-crossing closed loop has no well-defined interior, so phase 1 refuses
// the cut rather than guessing which sub-loop the user meant.
//
// The test is a segment-intersection pass over the projected polyline, skipping
// adjacent segments (which share an endpoint by construction) and, for a closed
// stroke, the first-against-last pair for the same reason.
bool draw_cut_self_crossing(const DrawCutStroke& stroke);

// True when the ruled strip FOLDS: the rails cross because the stroke turns with
// a radius tighter than the ruling reaches sideways.
//
// TWO reaches matter, and phase 1 only knew about the first:
//
//  1. The OUTWARD rail, pushed back out of the face by Extension E along the
//     ruling. At angle 0 the ruling is the inward normal, so that push is
//     straight out of the surface and moves the rail NOWHERE sideways - which is
//     why phase 1's test is `E * kappa > 1` only where the turn centre is on the
//     outward side (a concave corner). At angle theta the ruling leans sideways by
//     sin(theta), so the outward rail's lateral offset is E * sin(theta) and the
//     inward rail's is D * sin(theta) the other way - and the INWARD one is the
//     dangerous one, because D is the depth and can be tens of millimetres.
//
//  2. So the reach to test is max(E * |sin theta|, D * |sin theta|) on the side
//     the lean goes, plus phase 1's E on the outward side. A concave stroke with
//     a large angle folds where the spec says it does.
//
// `depth` is the reach the cut will actually use, in mm (through-all's derived
// reach, or the Depth slider). `angle_deg` is the draft angle. A fold is a
// WARNING, not a refusal: Manifold tolerates a slightly self-intersecting cutter
// and mcut is the fallback, so a missed fold degrades to "boolean failed ->
// complement recovery" rather than to a wrong cut. `worst_kappa`, when non-null,
// receives the largest curvature found so the panel can say how tight.
bool draw_cut_strip_folds(const DrawCutStroke& stroke,
                          double               extension,
                          double*              worst_kappa = nullptr,
                          double               angle_deg = 0.0,
                          double               depth = 0.0);

// ---------------------------------------------------------------------------
// The split.
// ---------------------------------------------------------------------------

// Which side of a drawn cut is "upper":
//
//   CLOSED stroke: the cutter encloses a PLUG, and `upper` is that plug (the
//     INTERSECTION) - "upper" is the piece the stroke drew around. The binormal
//     sign (see DrawCutStroke::binormal) makes this independent of drag
//     direction.
//   OPEN stroke: the strip cuts the part in two with no inside or outside, so
//     the plane's own convention applies - the half on the +Z side of the cut
//     plane frame is `upper`, matching flat and curved. The gizmo's Swap-sides /
//     right-click flip gesture covers the rest.
//
// cut_with_solid() already produces (inside, outside) as (lower, upper), so the
// CLOSED case is exactly the flip of that pair. draw_cut_split() does the flip,
// so its callers never have to know.
bool draw_cut_split(const indexed_triangle_set& mesh,
                    const DrawCutStroke&        stroke,
                    const DrawCutParams&        params,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    DrawCutError*               err = nullptr);

// Which side a drawn cut would leave empty, cheaply, before running two
// booleans. The analogue of curved_cut_empty_sides(): the stroke's cutter is a
// solid, so the test is "is any vertex of the mesh inside it, and is any outside
// it" - done with a winding-number-free parity ray cast along +Z against the
// cutter's own triangles, which is cheap because the cutter is a few thousand
// faces and the answer short-circuits as soon as both sides have a vertex.
//
// `thickness` rides along exactly as it does for the curved test: with a kerf the
// sides are tested against the OFFSET cutters, so a side counts as empty when the
// kerf has eaten everything that was on it.
void draw_cut_empty_sides(const indexed_triangle_set& mesh,
                          const DrawCutStroke&        stroke,
                          const DrawCutParams&        params,
                          bool&                       upper_empty,
                          bool&                       lower_empty);

} // namespace Slic3r

#endif /* slic3r_DrawCut_hpp_ */
