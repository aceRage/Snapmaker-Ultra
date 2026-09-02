# Flat-top cap fix wave — implementation report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `f3075afc50` (absorb-tail
fix wave 2, landed on top of the cap commit `8c5bf752de`). Answers
`flat-top-cap-review.md`'s I1/I2 + Minors 1/3/4, filed against `8c5bf752de`, re-verified against
the current HEAD — confirmed by reading absorb-tail-fixwave2-report.md and the current file that
the absorb-tail fix wave 2 touches the same descent loops but none of its own findings; the
`reach` opening (I-A) and `t_keep_core = t` (I-D) sites are untouched by this wave.

---

## 1. The bug, and the one fix for both findings

`exposed_surface_part()`'s wall-stack yardstick is POINTWISE: it measures each point's distance to
the *reference layer's own contour*, not the local slope of the patch the point belongs to. Two
consequences, one root cause:

- **I1** — a genuinely flat top/bottom that is not the object's own topmost/bottommost face (a
  ledge beside a taller riser) has a wall-stack-wide band nearest the riser that reads as "sloped"
  even though its own local slope is 0, so it never got capped: full D depth forever, zero tool
  changes saved on exactly the geometry the cap targets.
- **I2** — a slope whose per-layer staircase run `r` is only a little above one wall stack gets the
  *inner* wall-stack band capped and the *outer* `r - wall_stack` remainder kept (backwards for a
  dome), producing alternating capped/uncapped rings that the absorb then silently re-annexed, or
  not, depending on `gap_infill_speed`.

**Fix** (`MultiMaterialSegmentation.cpp`, new `flat_cap_component_ex()`, called from both the top
and bottom descent loops in place of the bare `exposed_surface_part()` call that used to build
`top_flat_cap_ex`/`bottom_flat_cap_ex`):

```cpp
static inline ExPolygons flat_cap_component_ex(const ExPolygons              &projected_patch,
                                                const std::vector<ExPolygons> &input_expolygons,
                                                size_t                         reference_layer_idx,
                                                size_t                         num_layers,
                                                float                          wall_stack_width)
{
    const ExPolygons exposed = exposed_surface_part(projected_patch, input_expolygons, reference_layer_idx, num_layers, wall_stack_width);
    if (exposed.empty())
        return ExPolygons{};
    return intersection_ex(projected_patch, offset_ex(opening_ex(exposed, wall_stack_width), 2.f * wall_stack_width));
}
```

**The component rule, as implemented (no new angle constant — the SAME wall-stack yardstick,
applied to the component instead of the point):** `exposed_surface_part()`'s own output (already
known to be farther than one wall stack from the reference contour) is *opened* at
`wall_stack_width`. A component survives the opening only if it has an actual flat core at least
`2 * wall_stack_width` wide (a ring or ledge at least 3 wall stacks total, ≈2.6 mm / ≤≈2.2° at
0.1 mm layers) — then the survivor is dilated back out by `wall_stack_width` plus the erosion
`exposed_surface_part()` already applied (`2 * wall_stack_width` total) and clipped to the patch:

- A component that survives (I1's ledge: 10 mm wide) is capped **whole, rim included** — the
  dilate-back specifically restores the near-riser band the pointwise test used to exclude.
- A component that does not survive (I2's 3/4/5° rings: `r - wall_stack` = 1.03/0.55/0.26 mm, all
  under `2*wall_stack_width` = 1.757 mm) returns **empty** — the whole ring stays wholly at D,
  exactly like any slope above the classic ~6.49° cliff, never partially capped.
- The topmost/bottommost origin's own half-ring (Minor 1: a patch with no reference layer at all is
  wholly "exposed" by `exposed_surface_part()`'s own early return) is itself narrower than
  `2*wall_stack_width` at every slope this feature targets, so it is opened away to empty too — the
  apex is never wrongly capped.

Both call sites (top `:2043-2045`→now via `flat_cap_component_ex`, bottom mirrored) and the
`t_keep_core`/`reach`-opening sites from absorb-tail fix wave 2 are untouched — confirmed by
`git diff` review (below) and by every existing `[paintdepth]`/`[chameleon]` test staying green.

---

## 2. RED evidence (real, stash-based — not simulated)

`git stash push -- src/libslic3r/MultiMaterialSegmentation.cpp` (production reverted to
`f3075afc50`, all new/modified tests left in place), rebuilt, ran `[paintdepth]`:

```
test cases:   92 |   87 passed |  5 failed
assertions: 1500 | 1460 passed | 40 failed
```

The 5 failing cases are exactly the 5 new ones this wave adds (the other 87 stayed green, matching
the pre-wave baseline exactly). Measured failures, independently corroborating the review's own
probe numbers almost to the decimal:

- **I1 ledge (top).** Depths 6-13 wrongly claimed at both the mid-ledge and near-riser probes
  (`:4923`).
- **I1 ledge (bottom mirror).** Depths 3-13 wrongly claimed (`:4952`).
- **Cost evidence on the ledge (Minor 3).** `painted_layers == 6` failed with actual **15** —
  matching the review's own "15 painted layers" measurement exactly.
- **I2 no-striping.** `claim_with_cap.size() == claim_cap_disabled.size()` failed **10 == 1** at
  3°/4° (both `gap_infill_speed` settings) — matching the review's own "~10 disjoint polygons"
  exactly. Reach: **11.4 vs 28.6 mm** at 3° — the review's own measured stripe boundary (11.40 mm)
  reproduced to the decimal, against the un-capped 15×r bound (28.62 mm).
- **Minor 1 apex (10/15/20°).** Failures **only** under the "slope 10 deg" section, at **exactly**
  layers 15-23 (9/9 layers failed; 14 and 24-26 did not) — the 15°/20° sections printed with zero
  failures, confirming they were never actually affected. At layer 20: reach **5.1 vs 5.35 mm** —
  the review's own "5.10 vs 5.39" reproduced almost exactly (small residual difference is the
  review's centre-point probe vs this test's cap-disabled-reference methodology, not a discrepancy
  in the underlying bug).

`git stash pop` restored the fix; rebuild + full re-verification below confirms GREEN.

---

## 3. Slope numbers across the full descent, before/after (Minor 1)

Rather than hand-derive the exact reach at every layer near the object's own apex (not a closed
form worth inlining — it depends on how many origins are actually available there), the committed
test compares each fixture's claim against an otherwise-identical build where
`top_shell_layers_override=15` makes the cap provably **inactive** while leaving
`top_descent_layers` itself unchanged (`max(shell, 15)` is 15 whether `shell` is 6 or 15) — so any
difference is caused only by the cap firing where it should not.

| slope | layers checked | BEFORE (reach_with_cap vs reference) | AFTER |
|---|---|---|---|
| 10° | 15-23 | differ by 0.25-0.30 mm every layer (e.g. layer 20: 5.10 vs 5.35) | **byte-identical, all 13 layers 14-26, tolerance 0.0001 mm** |
| 10° | 14, 24-26 | already identical pre-fix | unchanged |
| 15° | 14-26 | already identical pre-fix (smaller apex half-ring pre-erased by the existing `small_region_threshold` opening) | **unchanged, byte-identical** |
| 20° | 14-26 | already identical pre-fix | **unchanged, byte-identical** |

Layer-12 pin (existing "Slope regression pin" test, untouched): 10/15/20° = 1.4760095102 /
1.4364457003 / 1.4022825876 mm, unchanged.

---

## 4. Cost numbers on the ledge fixture (I1 / Minor 3)

40×40×4 mm slab, centred 20×20×4 mm tower (top variant) / stem (bottom variant), `walls = 3`,
0.1 mm layers, `top_shell_layers=4/0.6mm` (effective 6) / `bottom_shell_layers=3/0.0mm` (effective
3):

| probe | BEFORE | AFTER |
|---|---|---|
| mid-ledge (15 mm from centre, 5 mm clear of the riser) | capped at 6/3 layers (already correct) | capped at 6/3 layers |
| near-riser (10.4 mm from centre, 0.4 mm past the riser) | claimed through depth 14 (15 layers total) | capped at 6/3 layers |
| painted layers over the **whole** footprint (cost test) | **15** | **6** |

---

## 5. Tests added (`[paintdepth]`, 5 new `TEST_CASE`s + 1 existing test extended — 92 cases /
1500 assertions total, baseline 87/1197)

1. **I1 ledge (top).** New `slice_capped_ledge()` fixture (two stacked `ModelVolume`s, same
   construction style as `process_z_interface_cube()` — no boolean mesh needed): a 40×40×4 mm slab
   with a centred 20×20×4 mm tower above it, slab's own `TOP_CAP_FACE` painted. Asserts depths 0-5
   claimed and 6-14 not, at BOTH a mid-ledge probe and a near-riser probe (0.4 mm past the tower's
   edge).
2. **I1 ledge (bottom mirror).** Same fixture with the stem below a shelf, shelf's
   `BOTTOM_CAP_FACE` painted. Depths 0-2 claimed / 3-14 not, both probes.
3. **Cost evidence on the ledge (Minor 3).** Counts painted layers via `.empty()` on the whole
   claim (not a centre probe): `== 6`.
4. **I2 no-striping.** 3/4/5° × `gap_infill_speed` ∈ {default, 0} (6 sections): polygon count and
   `claim_reach_mm` compared against a same-depth cap-disabled reference build
   (`slice_bounded_frustum`'s new `top_shell_layers_override` parameter). No magic reach numbers.
5. **Minor 1 full descent.** 10/15/20° × layers 14-26 (13 layers each, spanning the required
   15-23 window with margin), each compared against the same cap-disabled reference,
   `WithinAbs(..., 0.0001)`.
6. **Absorb-safety test extended (Minor 4).** The existing "interior absorb does not annex a flat
   cap's capped floor" test now loops over `gap_infill_speed` ∈ {default, 0} (the widened kill
   width) via `DYNAMIC_SECTION`; both pass.

Two fixture-builder signature extensions, both backward-compatible (trailing, defaulted, every
existing caller unaffected): `slice_bounded_frustum` gained `gap_infill_speed` and
`top_shell_layers_override`; `slice_two_painted_colours` gained `gap_infill_speed`. One new
fixture-builder function, `slice_capped_ledge()` (plus a small `ledge_offset_probe()` helper) — no
existing fixture-builder function or mesh asset modified.

---

## 6. Process / gates — all on the FINAL committed-state binary

- Build-slot checked (`Get-Process cl,link,MSBuild`) before every build; free every time. All
  builds run in the foreground with generous timeouts, `MSBUILDDISABLENODEREUSE=1`, no background
  monitors.
- `[paintdepth]`: **92 cases / 1500 assertions, all green** (baseline 87/1197; +5 cases exactly
  matching the 5 new `TEST_CASE`s — the 6th change, the absorb-safety test, extends an existing
  case rather than adding one). Identical under default order and two `--order rand` seeds (1 and
  2): 92/1500 all three runs.
- `[chameleon]`: **133 cases / 605 assertions, exact, unchanged.**
- `ALL_BUILD`: **exit 0, zero error lines.**
- `spike/verify_paintdepth.sh` ×2 on the final binary: **17/17 both times**, `RESULT: ALL PASS` —
  unpainted byte-parity and determinism both hold. `paint_depth_mode = unlimited` stays untouched
  by construction: `top_cap_active`/`bottom_cap_active` both require `normal_shell`
  (`paint_depth_normal_mm > 0`, false in unlimited mode), and this wave only changes what those
  already-gated branches compute, not the gate itself — confirmed by every `pdmUnlimited` test in
  `[paintdepth]` staying green.
- **TRUE full `libslic3r_tests` suite** (plain, no filters): **520 test cases | 518 passed | 2
  failed as expected**; **51579 assertions | 51577 passed | 2 failed as expected**; exit 0.
  Identical under two `--order rand` seeds (1 and 2). The 2 expected failures are the same
  pre-existing `[!shouldfail]`-tagged `test_mixed_filament.cpp` cases every prior wave in this
  feature has recorded. 515 → 520 is exactly this wave's +5 new cases.

## Self-review (hand-walk, no subagents)

- Read the full diff of both touched files end to end via `git diff` (not just the sections
  written directly — the surrounding context at every edit site). Production diff: one new
  35-line-comment + 6-line-function insertion, two one-line call-site swaps
  (`exposed_surface_part` → `flat_cap_component_ex`), two comment corrections — no line removed
  that wasn't a comment being corrected, no existing logic altered. The bottom loop's OTHER
  `exposed_surface_part(bottom_ex, ...)` call (the legacy `bottom_exposed_ex` path, gated on
  `!normal_shell`) is untouched, correctly — that call site is unrelated to this fix.
- Grepped every added `+` line across both files for `printf|cout|cerr|#if 0|if (false|TEMP|DIAG|
  UNGATE|TODO|FIXME|XXX|BOOST_LOG|getenv|dump|debug`: **zero matches**.
- **Hand-walk, slab+tower ⇒ whole flat top incl. the ledge capped at 6 layers**: confirmed by test
  §5.1/5.3 GREEN (mid-ledge and near-riser both capped at depth 6 top / 3 bottom; whole-footprint
  cost count = 6, was 15).
- **Hand-walk, 4° slope ⇒ one decision for the whole component, no stripes**: confirmed by test
  §5.4 GREEN for the `4 deg` sections specifically (both `gap_infill_speed` settings): polygon
  count and reach match the cap-disabled reference exactly; RED run showed 10 disjoint polygons /
  11.4 mm-truncated reach pre-fix at this exact slope.
- **Hand-walk, 15° slope ⇒ every layer identical to before**: confirmed by test §5.5 GREEN across
  layers 14-26 (13 layers), and by the RED run itself showing **zero** failures under "slope 15
  deg" even pre-fix — this slope was never actually broken, and the fix provably does not disturb
  it either.

## Commit

One commit, `fix(paint-depth):` prefix, touching `src/libslic3r/MultiMaterialSegmentation.cpp`
(production, purely additive except the two call-site swaps and two comment corrections),
`tests/libslic3r/test_paint_depth_clamp.cpp` (5 new test cases, 1 extended, 2 fixture-builder
signature extensions, 1 new fixture-builder + probe helper — every change backward-compatible or
additive), `docs/superpowers/specs/2026-08-31-paint-depth-design.md` (Cost note corrected from the
pointwise to the per-component rule), `.superpowers/sdd/2026-08-31-paint-depth/progress.md`, plus
this report. Fixtures (mesh/STL assets, existing fixture-builder *behaviour*) untouched — every
existing caller of `slice_bounded_frustum`/`slice_two_painted_colours` is byte-identical (new
parameters are trailing and defaulted); `slice_capped_ledge` is a genuinely new fixture-builder
function, not a modification of one. Defaults untouched.
