# Flat-top cap — implementation report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `9227fa72b1`.
Implements the user decision recorded in `progress.md` (2026-09-01): cap a FLAT painted
top/bottom claim at the effective solid-shell depth instead of the full D-driven
normal-thickness depth, since only the solid shell is ever visible — the rest is hidden
sparse infill of the same colour either way (`paint_infill_override`). Slopes and walls
keep the full D bound untouched.

---

## 1. Discriminator

Reused the existing `exposed_surface_part()` wall-stack yardstick verbatim — no new angle
constant. That function already answers "is this part of a patch farther than one wall
stack from the neighbouring layer's own contour", which is exactly "is this part of the
patch flat enough (local staircase run `r >= wall_stack`, ~6.49° at 0.1 mm layers) to have
been let through by the pre-Option-N gate". N1 (Wave B) made `top_exposed_ex` skip that
call entirely under `normal_shell` so slopes could reach D; this change calls the SAME
function a second time, unconditionally of `normal_shell`, purely to build a new
`top_flat_cap_ex` / `bottom_flat_cap_ex` polygon set used only by the cap — it never
touches `top_exposed_ex`/`bottom_exposed_ex` itself, so N1's slope behaviour is untouched
by construction.

Mechanism (`MultiMaterialSegmentation.cpp`, both descent loops, symmetric):

```cpp
const bool       top_cap_active  = normal_shell && stat.top_descent_layers > stat.top_shell_layers;
const ExPolygons top_flat_cap_ex = top_cap_active
    ? exposed_surface_part(top_ex, input_expolygons, layer_idx + 1, num_layers, wall_stack)
    : ExPolygons{};
...
// inside the per-step descent loop, after `last` (eroded term ∪ exposed/full-width term)
// is assembled for this depth:
if (top_cap_active && ! top_flat_cap_ex.empty() && int(layer_idx) - last_idx >= stat.top_shell_layers)
    last = diff_ex(last, top_flat_cap_ex);
```

`top_cap_active` gates the (otherwise wasted) `exposed_surface_part` call to only the case
where D genuinely reaches deeper than the shell. The subtraction applies to `last` AS A
WHOLE — both the eroded term and the exposed/full-width term — not merely to the
full-width contribution: a sufficiently WIDE flat cap's eroded term is still non-empty
well past the shell depth (offset accumulates ~0.88 mm/layer; a 40×40 mm cap isn't fully
eroded until ~depth 22), so subtracting only from the full-width term would have left a
shrinking core of "legacy eroded" material limping on to the full D depth — exactly the
extra cost this change exists to remove. Because `top_flat_cap_ex == top_ex` on a genuine
flat top (no neighbouring layer at all) and every component of `last` is a subset of
`top_ex`, the subtraction empties `last` completely once beyond the shell depth, and the
already-relaxed N3 break (`if (last.empty()) { ... break; }`) then stops the descent dead
— no wasted Clipper work continuing to the old 15-layer bound on a pure flat cap.

On a patch that is flat in the middle and rolls over to steep at its rim (a dome crown),
the split is POINTWISE — `exposed_surface_part` already does this correctly (documented in
its own header) — so the crown's contribution is capped while the rim/flank's contribution
(non-empty `top_ex \ top_flat_cap_ex`) is untouched and keeps descending to the full D
bound, verified by a dedicated test (§4).

Bottom loop mirrors exactly (`bottom_cap_active`, `bottom_flat_cap_ex`, condition
`last_idx - layer_idx >= size_t(stat.bottom_shell_layers)`).

Diff: 53 lines added to `MultiMaterialSegmentation.cpp`, purely additive (no line removed,
no existing line changed) — grepped for `if (false`, `TEMP`, `#if 0`, `printf`/`cout`/
`cerr`, `DIAG`, `UNGATE`, `TODO`, `FIXME`, `XXX` across the diff: zero matches.

---

## 2. RED evidence

Real TDD RED, obtained by `git stash push -- src/libslic3r/MultiMaterialSegmentation.cpp`
(production code only, tests left in place), rebuild, run — not simulated:

```
Failed 5 test cases, failed 32 assertions.
```

- Flat-cap test: depths 6–14 wrongly claimed at the cap's centre (9 failures).
- Dome/frustum crown test: same 9 failures at the crown's own depths 6–14 (the flank
  assertions, T1's own reach probes, passed even pre-fix — confirming the flank behaviour
  predates and is independent of this change).
- Bottom-mirror test: depths 3–14 wrongly claimed (12 failures, effective bottom shell = 3
  on this fixture).
- Absorb-safety test: the capped-floor centre point wrongly read as Extruder2 (1 failure);
  the "does colour B annex it instead" check already passed pre-fix (no absorb regression
  to begin with — see §5).
- Cost-evidence test: `painted_layers == 6` failed with actual `15` — the exact pre-fix
  D-driven depth.

The slope-pin test (10/15/20°, §3) was **not** in the failure list — it passed both before
and after, as a true regression pin should.

`git stash pop` restored the production fix; rebuild + full re-verification (§6) confirms
GREEN.

---

## 3. Slope numbers — before/after

Pinned via a deliberately tightened tolerance (`WithinAbs(expected, 0.0000001)`) run
against both the reverted (pre-fix) and restored (post-fix) production code, then reverted
to the real `0.03` tolerance for the committed test. Digits below are the actual captured
`normal_mm` values in both builds — byte-identical to 10 decimal places, not merely "within
tolerance":

| slope | reach (mm) | normal thickness (mm), BEFORE | normal thickness (mm), AFTER |
|---|---|---|---|
| 10° | 8.50 | 1.4760095102 | 1.4760095102 |
| 15° | 5.55 | 1.4364457003 | 1.4364457003 |
| 20° | 4.10 | 1.4022825876 | 1.4022825876 |

Matches wave-b-report.md's historical measured figures (1.476 / 1.436 / 1.402 mm) exactly.
No drift.

---

## 4. Tests added (`[paintdepth]`, 6 new `TEST_CASE`s, 80 cases / 1153 assertions total —
baseline 74/1056)

1. **Flat-top cap (RED headline).** `slice_bounded_frustum(40., 40., 4., TOP_CAP_FACE,
   pdmWalls, 3, 0.1, print)` — `bottom == top` degenerates the frustum to a plain
   vertical-walled prism (no new fixture). Depths 0–5 (6 layers, the effective shell)
   claimed at the cap's centre; depths 6–14 (up to the old D-driven 15) not.
2. **Slope regression pin (§3).** 10/15/20° frustums (same family as "normal thickness
   across slopes"), pinning the exact measured digits.
3. **Dome/frustum mix.** 15° frustum (T1's own fixture) with BOTH `TOP_CAP_FACE` and
   `FRUSTUM_SLOPED_WALLS` painted together. Crown: centre claimed at depths 0–5, not at
   6–14 (same numbers as test 1). Flanks: T1's own probe (layer 12, insets 3.0/5.0/6.0 mm)
   reproduced unmodified — proven unreachable by the crown's own descent by construction
   (crown's deepest reach from layer 29 is layer 15, never reaching layer 12).
4. **Bottom mirror.** Same fixture, `BOTTOM_CAP_FACE`. Effective bottom shell = 3
   (`bottom_shell_thickness` disabled by default) — depths 0–2 claimed, 3–14 not.
5. **Absorb safety.** Reused the existing "Important 2" fixture
   (`slice_two_painted_colours`, `TOP_CAP_FACE` = Extruder2 cap, `PLUS_X_FACE` = Extruder3
   stripe right up to the shared edge) — a genuinely ACTIVE painted neighbour at every
   layer, the precondition the interior inter-claim absorb needs to have a winner
   candidate. At depth 8 (past the cap, past Extruder3's own ~1.44 mm lateral reach), the
   box centre is claimed by NEITHER Extruder2 nor Extruder3.
6. **Cost evidence.** Counts painted layers on the flat-cap fixture: `== 6`. Pre-fix this
   read `15` (captured in the RED run, §2) — the 9-layer, ~2.5 cm³-purge recovery the spec's
   Cost note now records.

All 6 pass GREEN; the RED behaviour for tests 1, 3, 4, 5, 6 was independently confirmed via
the stash-based revert (§2), not merely inferred from the fix landing.

---

## 5. Absorb interaction

Verified with a real test (§4.5), not assumed. Reasoning for WHY it holds, confirmed by
the passing test: the interior inter-claim absorb only fires on a connected base component
that fails an `opening_ex(component, min_claim_width/2)` thinness test. My cap either (a)
removes an ENTIRE contiguous painted contribution at a whole-layer boundary (a clean
step-function, not a gradual thinning — confirmed by hand-trace in §1: `last` empties
completely, not partially, once the subtraction fires on a genuinely flat patch), or (b)
does nothing at all on sloped patches. Neither produces a newly-thin sliver for the absorb
to misjudge. The fixture used (§4.5) puts an ACTIVE painted neighbour immediately adjacent
to the capped floor specifically to give the absorb a winner candidate if it were ever
going to misfire, and it does not.

---

## 6. Process / gates — all on the FINAL committed-state binary

- Build-slot checked (`Get-Process cl,link,MSBuild`) before every build in this task; only
  once found real compile work in flight (a short-lived `cl.exe`, already exited by the
  time of the follow-up check — not this task's own process, not a collision). All builds
  run in the foreground with generous timeouts, no background monitors.
- `[paintdepth]`: **80 cases / 1153 assertions, all green** (baseline 74/1056; +6
  cases/+97 assertions, exactly this wave's additions). Identical under default order and
  two `--order rand` seeds (1 and 2): 80/1153 all three runs.
- `[chameleon]`: **133 cases / 605 assertions, exact, unchanged.**
- `ALL_BUILD` (scratchpad `build_next_wt.bat`): **exit 0, zero error lines**, twice
  (once after the fix landed, once again after the tolerance diagnostic was reverted to
  its final committed value).
- `spike/verify_paintdepth.sh` ×2 on the final binary: **17/17 both times**, including
  unpainted byte-parity vs the frozen pre-feature baseline and run-to-run determinism —
  expected, since this whole change is behind `is_mm_painted()` and `normal_shell`
  (painted-colour-only, `paint_depth_mode != unlimited`), never reached for unpainted
  objects.
- **TRUE full `libslic3r_tests` suite** (plain, no `--warn NoAssertions`): **508 test
  cases | 506 passed | 2 failed as expected**; **51232 assertions | 51230 passed | 2
  failed as expected**; exit 0. The 2 expected failures are the same pre-existing
  `[!shouldfail]`-tagged `test_mixed_filament.cpp` cases every prior wave in this feature
  recorded (now at lines 3483/4429, shifted ~14 lines by unrelated work elsewhere in that
  file — same test class, untouched by this change). 502 → 508 is exactly the 6 new cases.

## Self-review (hand-walk, per the task's binding checklist)

- **Flat cap → 6 layers.** Traced in §1 and independently confirmed by the cost-evidence
  test's own count (6, exact) and by the RED run showing the pre-fix count was 15.
- **15° slope → 1.436 mm.** `r = 0.3732 mm < wall_stack = 0.878540 mm` at 15°/0.1 mm ⇒
  `exposed_surface_part` returns EMPTY ⇒ `top_flat_cap_ex` empty ⇒ the cap's
  `! top_flat_cap_ex.empty()` guard never opens ⇒ `last` never modified by this change at
  any depth. Confirmed to 10 decimal places in §3 (1.4364457003, identical before/after).
- **Dome → crown 6 layers, flanks D.** Confirmed by test 3 (§4): crown depths 0–5 claimed /
  6–14 not (same as the plain flat-cap test); flank probe at layer 12 reproduces T1
  (3.0/5.0 mm claimed, 6.0 mm not) unmodified, proven geometrically unreachable by the
  crown's own descent (crown's deepest possible reach stops at layer 15, short of layer
  12) rather than merely observed to still pass.

## Commit

One commit, `feat(paint-depth):` prefix, touching
`src/libslic3r/MultiMaterialSegmentation.cpp` (production, 53 lines added),
`tests/libslic3r/test_paint_depth_clamp.cpp` (206 lines added), and
`docs/superpowers/specs/2026-08-31-paint-depth-design.md` (Cost note updated to record the
cap and the recovered cost), plus this report and a `progress.md` entry. Fixtures
(mesh/STL assets, existing fixture-builder functions) untouched — every new test reuses an
existing fixture builder (`slice_bounded_frustum`, `slice_two_painted_colours`) rather than
adding a new one. Defaults untouched.
