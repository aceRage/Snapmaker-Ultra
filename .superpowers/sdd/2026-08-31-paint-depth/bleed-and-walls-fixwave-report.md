# Bleed-and-walls fix wave — report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, parent `cfe7fae1df`.
Inputs: `outward-bleed-investigation.md`, `wall-count-investigation.md`. Four user-visible
defects confirmed in a real GUI slice, fixed in one commit.

**Status: all four landed, all gates green.** No GUI validation performed (see §7).

---

## 1. F1 — exterior paint bleed (regression we introduced in `65d17c964f`)

**Anchors:** `MultiMaterialSegmentation.cpp` top descent (~`:1720`), bottom descent (~`:1824`).

The full-width term was `top_exposed_ex ∩ layer_slices_trimmed` — bounded only by a layer
outline, never inset. Wherever the painted patch reaches the silhouette (every painted flat or
chamfered cap, because `exposed_surface_part`'s early return at `:1331-1332` hands such a patch
back with *no* clearance test), the claim reached the contour exactly on every sub-surface shell
layer, painting the exterior perimeter of a 0.7–1.0 mm ring of side wall the user never painted.

**Fix (investigation's Option A):** intersect the full-width term with
`offset_ex(input_expolygons[last_idx], -wall_stack)`, `wall_stack = extrusion_spacing +
extrusion_width` (0.87854 mm at 0.45 mm walls / 0.1 mm layers; 0.85708 mm at 0.2 mm).

- Constant one-wall-stack inset, not the legacy growing `k × wall_stack` (4.4 mm by depth 5) —
  so it stays strictly more generous than pre-feature behaviour.
- Measured from the **descent layer's own contour** (`last_idx`), not from the patch and not
  from `layer_idx`. That is what makes it correct for objects that narrow downward (undercut,
  waist, overhang below a painted top), where a patch clear of the surface layer's contour can
  still land on a lower layer's contour. This is the hole that made Option B unsafe.
- The surface layer is untouched — appended separately with zero margin, because that is the
  facet the user actually painted.

**Deliberate trade-off, unchanged from the investigation's own analysis:** a fully painted flat
top on a *plain box* loses its painted exterior side-wall ring below the top layer, reverting to
upstream appearance there. This is the only regression against `65d17c964f`'s approved intent
and it is confined to paint that reaches the silhouette. An interior painted feature (the user's
8 mm eye/cheek, a raised boss, a flat shelf) is not clipped at all and keeps its **entire**
footprint at every shell layer — pinned by a dedicated test.

### 1a. The I1 absorb decision: REMOVED

The investigation said Option A "subsumes and obsoletes" the `cfe7fae1df` absorb
(`diff_ex(base_rest, opening_ex(base_rest, 0.5·wall_stack))`) and offered "delete, or leave as
dead-but-harmless". **Deleted, at both sites**, and the honest reason is stronger than
redundancy:

- With F1, `last` is a subset of the layer contour eroded by one wall stack on **both** terms
  (legacy term: inset `k·wall_stack`, `k ≥ 1`; F1 term: inset `1·wall_stack`). So `base_rest` is
  an annulus at least one wall stack wide **by construction** — the absorb's diff is empty.
- It is not merely dead, it is dead *on a knife-edge*. `opening_ex(base_rest, 0.5·wall_stack)`
  against an annulus of width exactly `wall_stack` is precisely the marginal case where Clipper's
  arc approximation decides whether the ring survives its own opening. Any residue it found would
  be `append`ed straight back onto the contour — re-creating the exact bleed F1 removes, sporadically
  and geometry-dependently. Leaving it would be two mechanisms silently fighting over one invariant.

Same invariant ("base material at the contour is either nothing or at least one wall stack
wide"), one mechanism, enforced from the generous side instead of the absorbing side.

The test that pinned the absorb's behaviour (the chamfered-frustum-cap case) keeps its fixture
and subject and has its probes **flipped**, with the reversal documented in place.

---

## 2. F2 — the lateral clamp silently self-disabled on thin geometry

**Anchor:** new `paint_depth_clamp_keep_core()` (`MultiMaterialSegmentation.cpp` ~`:1146`), used
by `cut_segmented_layers`.

`diff_ex(claim, offset_ex(layer, -cut_width))` — where local half-thickness `t < cut_width` the
erosion is empty, `diff_ex(claim, ∅) == claim`, and the clamp is a **total no-op**. Not a partial
weakening: an on/off cliff, silent. At 4–6 mm it holds across most of a typical organic model,
which is why increasing the depth stopped deepening the paint and started revealing the raw
Voronoi partition instead.

**Semantics chosen — halving ladder with a doubled membership test.** Where a full-`band` inset
leaves no core, fall back to the widest `b` from `band/4, band/8, … band/128` whose **twice**
value the local geometry supports (`opening_ex(layer, 2b)` non-empty there). A part of local
half-thickness `t` therefore gets

    2b ≤ t < 4b   ⇒   t/4 < b ≤ t/2

- `b ≤ t/2` is load-bearing: the base core left behind is `t − b ≥ t/2`, so **at least half of
  the local cross-section stays base-coloured**. The claim becomes proportionate to the geometry
  instead of swallowing it whole.
- `b > t/4` bounds how much paint the degradation gives away.
- Testing membership at `2b` rather than `b` is what buys the first bound — at radius `b` alone,
  a part with `t` barely above `b` keeps a vanishing core, i.e. the no-op again.
- **Thick geometry is bit-for-bit untouched**: where the full-band erosion is non-empty, `thin`
  is empty, the ladder never runs, the core is exactly today's `offset_ex(layer, -band)`.

**Degenerate limit (stated as required):** the ladder is bounded at six steps, so its last
membership radius is `band/64`; a part thinner than that keeps the old no-op — 0.022 mm at a
1.44 mm walls-mode band, 0.094 mm at a 6 mm one. Both are far below one extrusion, i.e. geometry
that cannot carry two colours through its thickness under any clamp.

**What this does NOT do — recorded honestly.** It bounds the claim; it does not stop a Voronoi
cell **wrapping onto an opposite face**. The clamp keeps whatever lies within `band` of *any*
boundary, so where a painted surface's cell reaches around a rounded fin tip, the far face's own
perimeter band survives for any `b > 0`. The investigation's §2.3 wording ("touches the opposite
exterior face") is marked `[I]`; on reading the mechanism, suppressing that needs a clamp measured
from the **painted** boundary rather than from the layer contour — a different mechanism, out of
scope here. The test therefore asserts the claim is proportionate (a base core survives) rather
than asserting the opposite face is unpainted, and says so.

**Cost.** +1 dilation +1 diff per painted layer in the common case; ≤6 ladder steps only where
thin parts exist. Partly pre-paid: the erosion was hoisted out of the per-extruder loop, where it
was being rebuilt from identical inputs once per extruder.

---

## 3. F3 — "N walls" now delivers N wall loops

**Anchors:** `PaintDepth.cpp` / `PaintDepth.hpp`, call site `MultiMaterialSegmentation.cpp`
~`:2645`, `paint_depth_walls` tooltip.

    band(N) = N·perimeter_spacing + 2·(ext_perimeter_width − ext_perimeter_spacing) + 0.25·perimeter_spacing

replacing `ext_perimeter_width + (N−1)·perimeter_spacing`. The signature gained
`ext_perimeter_spacing`; the call site passes `region.flow(frExternalPerimeter).spacing()`.
Result is clamped `≥ 0` so a degenerate flow collapses to "disabled" rather than going negative.

At 0.45 mm lines / 0.1 mm layers: **N=1 → 0.578595, N=3 → 1.435675, N=6 → 2.721294** (old N=3
was 1.307080). Downward margin at N=3 goes 0.083 mm → **0.212 mm, ≈2.5×**, and is now
parity-independent.

**DOCUMENTED, NOT FIXED — the hard ceiling.** `LimitedBeadingStrategy` caps Arachne at
`max_bead_count = 2 × wall_loops` (`WallToolPaths.cpp:514`,
`LimitedBeadingStrategy.cpp:41-64`). At the stock `wall_loops = 2`, **a painted region can never
exceed 4 loops however wide the band is.** Band beyond that ceiling is not lost — it becomes
painted solid/sparse infill inside the same painted region, i.e. still painted material — but it
is not extra *loops*. Recorded in `paint_depth_band_mm`'s header and now in the
`paint_depth_walls` tooltip. Practical consequence for the user: `paint_depth_walls = 5` (their
~2 mm target) needs `wall_loops ≥ 3`.

`paint_depth_walls` / `paint_depth_mm` **defaults untouched** (walls = 3 ≈ 1.44 mm, mm = 1.5),
per the standing user decision.

---

## 4. F4 — interlocking no longer costs a wall loop on alternating layers

**Anchors:** new `paint_depth_interlocking_depth_mm()` (`PaintDepth.cpp`), call site
`MultiMaterialSegmentation.cpp` ~`:2664`, `PrintConfig.cpp` default + tooltip.

`cut_segmented_layers:1164/:1169` narrows the band by the interlocking depth on even layers. Our
0.3 mm Stage-1 default is ≈0.70·spacing — most of a whole bead-count window and 3.6–5.3× the
band's margin — so the painted region dropped from N loops to N−1 on every even layer: the user's
3/2/3/2 "only 1-2 walls".

**Two changes, both needed:**

1. **Clamp** the effective depth to `min(configured, 0.25 · perimeter_spacing)` — exactly the
   count-window margin F3 builds into the band, so the notch is guaranteed to fit *inside* that
   margin and cannot move Arachne's strip across a bead-count boundary. 0.107 mm at 0.45/0.1,
   0.102 mm at 0.45/0.2.
2. **Default lowered 0.3 → 0.1 mm**, so the clamp is normally inert and only bites for users who
   raise it by hand. This is the only default changed in this commit.

The band was **not** widened to compensate — the investigation proved that just converts 3/2
alternation into 4/3.

At the call site the clamp uses the **minimum** perimeter spacing across printing regions (the
opposite conservative direction from `max_width`'s max): the notch must stay inside the margin of
the narrowest-walled region, whose margin is smallest in absolute millimetres.

Tooltip now documents both the cap and why it exists.

### Hand-walked bead counts (both parities, both common layer heights)

| | 0.1 mm layers | 0.2 mm layers |
|---|---|---|
| band(3) | 1.435675 | 1.408850 |
| odd-layer `x = T − 2·s_ext` | 0.535675 | 0.508850 |
| even-layer `x` (notch 0.1) | 0.435675 | 0.408850 |
| 3-bead window | [0.323797, 0.647560) | [0.307590, 0.615140) |
| **beads, both parities** | **3** | **3** |

---

## 5. Tests

TDD, real RED per item, `[paintdepth]` tag, existing harness reused/extended.

**RED run** (signatures scaffolded with pre-fix bodies, so failures are on behaviour not
compilation): **10 cases / 57 assertions failing** —

- F1: painted flat cap (new), chamfered/tapered cap (flipped), small painted top feature, small
  painted bottom feature.
- F2: thin-fin base core (new), both layer parities.
- F3: band formula (symbolic for N=1/3/6, the investigation's literals, per-wall pitch delta),
  walls-mode edge cases.
- F4: notch cap helper, "notch never eats the N-bead budget" (walls 1–6 × configured 0.1/0.3/1.0),
  and the geometric both-parities test.
- The F1 **interior-boss guard passed in RED as designed** — it guards the approved intent rather
  than driving a fix, and its passing in both builds proves the fixture is sound.

**New/changed fixtures:** `slice_painted_box()` (arbitrary XY footprint + pinned inner/outer wall
widths and layer height — the 40×40 cube cannot express a cross-section thinner than the band);
an interior-boss model (40×40×3.8 slab + centred 8×8×0.4 boss, painted boss cap, so the shell
descent runs past the boss into the slab where the claim is 16 mm clear of the silhouette).

**F4 assertion method — stated as required:** bead counting is not reachable from this harness
(these fixtures stop at `PrintObject::slice()`, which never runs perimeter generation), so the
parity assertion is made on **band width**, probed geometrically at the exact N-bead optimum
thickness `N·s + 2(ext_w − ext_s)` on both an even and an odd layer, plus a pure-arithmetic pin
that `band(N) − interlock_eff ≥ N·s + 2(ext_w − ext_s)` for N = 1…6.

**GREEN:** `[paintdepth]` **36 cases / 416 assertions, all pass** (baseline 30/277 — grown by the
new tests, none removed). `[chameleon]` **133 cases / 605 assertions**, exactly at baseline.

---

## 6. Gates

| gate | result |
|---|---|
| `[paintdepth]` | 36 / 416, all pass (baseline 30 / 277) |
| `[chameleon]` | 133 / 605, unchanged from baseline |
| ALL_BUILD (scratchpad `build_next_wt.bat`) | **exit 0** |
| `spike/verify_paintdepth.sh` run 1 | **17/17 ALL PASS** |
| `spike/verify_paintdepth.sh` run 2 | **17/17 ALL PASS** |
| unpainted byte-parity vs frozen baseline | **holds** (both runs, both slices) |

`verify_paintdepth.sh`'s `defaults-mmu_segmented_region_interlocking_depth` expectation was
updated 0.3 → 0.1 to match F4 (plus two header comments). Unpainted byte-parity is unaffected:
that key is already in the script's normalize strip-set, and the whole
`cut_segmented_layers` / descent path is gated behind `is_mm_painted()` and never reached for an
unpainted object.

---

## 7. Concerns

1. **No GUI validation.** Per `outward-bleed-investigation.md` §6 the decisive fixture is a
   painted flat-topped object **with a chamfered or filleted top edge** — a plain cube proves
   nothing. Expected: clean base-coloured exterior on every sub-surface layer, no hairline ring,
   no groove; interior painted features still full width at full shell depth.
2. **F1's deliberate trade-off needs a user read.** On a plain painted-flat-top box the exterior
   side wall below the top layer is now base-coloured (upstream appearance). Correct per the
   investigation, but it is a visible change and could be misread as "paint too shallow".
3. **F2 does not stop tip wrap-around** (§2). If the user's "outward" symptom was specifically
   colour appearing on a *far* face at a fin tip, this wave reduces but does not eliminate it.
4. **Classic wall generator is untouched.** `wall-count-investigation.md` §2d: on
   `wall_generator = classic`, a 3-wall band yields 2 external-width loops + one gap-fill line,
   always, on every layer. The F3 band is Arachne-shaped; classic needs its own fix. **Worth
   asking the user which generator they are on** — if classic, that is a separate and larger
   defect than anything in this wave.
5. **`2 × wall_loops` ceiling** (§3) — documented, not fixed. Users wanting >4 painted loops must
   raise `wall_loops`.
6. **F2 cost** is bounded but real (§2); worst case is a model made entirely of sub-band fins.

---

## 8. Flags for the approved next piece of work (curved-surface gap)

1. **F1 is exactly neutral on the curved gap.** Everything F1 changed lives *inside* the
   `if (! top_exposed_ex.empty())` guard, and on slopes above the ~6.5°/13.1° cutoff
   `exposed_surface_part` returns empty and the block never executes. Clean slate there.
2. **But F1 constrains the shape of the fix.** If the follow-up widens
   `exposed_surface_part`'s acceptance so shallow-but-not-flat slopes qualify, those newly
   qualifying patches immediately inherit F1's one-wall-stack inset. That is the *right*
   behaviour (no exterior bleed) — but it means such a follow-up cannot deliver extra depth **at
   the silhouette** on a curved face, only inward of one wall stack. Any design that intends to
   own the outer perimeter of a sub-surface layer on a curved face is in direct conflict with F1
   and would reintroduce the bleed the user reported. Worth settling that up front.
3. **The above-~24° arm is untouched and still the live lever**:
   `top_ex = opening_ex(top_ex, small_region_threshold)` (`:1618`, bottom twin `:1690`), which
   deletes per-layer top strips narrower than 0.225 mm and so annihilates the claim for
   `θ > atan(h/0.225)` (23.96° at 0.1 mm).
4. **F2 now caps lateral depth on thin geometry.** A slope-aware lateral-depth follow-up that
   *increases* the band on shallow slopes will, on locally thin geometry, be capped at `b ≤ t/2`
   by the ladder rather than growing without limit. On thin curved shells — the common organic
   case — increasing the lateral band past twice the local half-thickness now buys nothing. This
   is a genuine interaction to design around, not a bug.
5. **F3 helps the shallow-slope opacity gap slightly and for free**: the walls-mode band grew
   ~9.8 % (1.307 → 1.436 mm at 0.45/0.1), and normal painted thickness on a slope is `band·sin θ`,
   so the whole `check-5` curve shifts up by that factor. It does not close the gap, and the
   `2 × wall_loops` ceiling still applies.
