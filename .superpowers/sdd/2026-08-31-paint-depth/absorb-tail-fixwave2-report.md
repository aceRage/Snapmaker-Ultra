# Absorb-tail fix-wave 2 — implementation report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `8c5bf752de` (flat-top
cap). Answers `absorb-tail-fixwave-review.md`'s I-A/B/C/D + Minor, filed against `9227fa72b1` and
re-verified against the current HEAD (the flat-top cap touches the same descent loops but none of
its own findings — confirmed by reading its own diff before starting).

---

## I-A — leak-test floor, no longer tuned; the Extruder-3 ring explained

**Floor.** `has_painted_unopened_fragment` (test file) dropped `min_area_mm2` entirely — no floor
at all, matching `has_interclaim_sliver`'s own discipline (a width test via `opening_ex`, not an
area test). `extruder_id` is now a real parameter instead of a hard-coded `{2}` loop.

**Fix.** `segmentation_top_and_bottom_layers`'s shadow-building site (where `legacy_top_and_
bottom_layers_out[color_idx][layer_idx]`/`reach` is assembled from the raw, un-opened descent
contributions) now opens `reach` with the SAME per-layer/per-colour `small_region_threshold`
`full`'s own band-level opening already uses, gated on `stat.normal_shell` (a no-op in unlimited
mode and for colour 0, by construction). This is complementary to, not a replacement for, the
previous wave's subtract-only clip (`full = diff_ex(full, intersection_ex(excess, other))`):
subtract-only stops the clip from ADDING area; opening `reach` stops the region the clip
deliberately EXEMPTS (`full ∩ legacy`) from still carrying an un-opened raw finger that could
shape a thin sliver out of `full`'s own (already wide) boundary.

**Result:** `has_painted_unopened_fragment(*object, /*Extruder2*/2)` is now EXACT — zero raw
sub-threshold survivors, no floor of any kind, on the sphere fixture RED confirmed before
(`has_painted_unopened_fragment(*object, 2)` true, unmodified 8c5bf752de production) and GREEN
after (see RED evidence below).

**Extruder-3 ring (4.0/3.5mm² at layers 146-148), explained not merely excluded.** First
hypothesis (sliced in `pdmUnlimited`, reasoning `normal_shell` is false everywhere so the whole
Wave-B/Item-1/Item-2/this-wave's-I1 machinery is provably inert) was **measured and refuted**: the
ring does **not** reproduce in unlimited mode. Re-diagnosed from the review's own positional
evidence (1.2mm inside the surface, within colour 2's legacy shell depth) as the interaction of
TWO mechanisms neither Wave B nor this fix-wave touches: (a) Stage 1's lateral clamp
(`cut_segmented_layers`, walls-mode only — `segmentation_max_width` is 0 in unlimited mode, so
this never runs there) narrows colour 3's lateral claim to a band near the surface; (b)
`merge_segmented_layers`'s per-extruder trim loop (`diff_ex(segmented_regions_trimmed, top_and_
bottom_by_extruder)`) — unconditional, present in every mode, unrelated to any paint-depth code —
then cuts that already-narrow band by colour 2's own top/bottom claim, leaving a thin ring
remnant. In unlimited mode (a) never narrows the band, so (b)'s remainder is wide (a genuine core),
not thin — it fails the width test for an unrelated reason, not because (b) stopped running.
Pinned BOTH ways: present at `pdmWalls` (a dedicated, un-hidden `CHECK`, not excluded by a floor)
and absent at `pdmUnlimited` (a dedicated `CHECK_FALSE`) — proving the dependency is on Stage 1's
own clamp, genuinely out of scope for this fix-wave's own I1 change.

---

## I-B — all four `paint_depth_solid_interfaces` gates proven by real mutation

New tests replace the metric, not the fixture: `region_perimeter_length` (recursive walk of
`layerm->perimeters`, summing every leaf's `length()` — `ExtrusionEntityCollection::length()`
throws by design) instead of `region_top_fill_area`'s `fill_surfaces` stTop sum. `perimeters` is
written ONLY by the perimeter generator and never touched again (`Layer.cpp`'s `make_perimeters`
call is its sole writer), so this metric — unlike `fill_surfaces`, rebuilt by `detect_surfaces_
type`/site 1 AFTER the perimeter generator runs — cannot be moved by sites 1/2, only by 3/4.

- Site 4 (Arachne, `PerimeterGenerator.cpp:2273`): new `len_on < len_off` pin on the existing
  site-4 sandwich fixture (`only_one_wall_top=true`).
- Site 3 (Classic, `:622`): new `len_on != len_off` pin, with the sandwich fixture's new trailing
  `min_width_top_surface_mm=0.0` parameter (registry default 300%/1.35mm otherwise swallows the
  exposed ring whole, as the previous wave found).

**Mutation results (each site ungated alone — `&& paint_depth_solid_interfaces` dropped —
rebuilt, tested, reverted; all four confirmed, production files verified byte-identical after):**

| Site | Location | Mutation result |
|---|---|---|
| 1 | `PrintObject.cpp:1338` (`detect_surfaces_type`'s `interface_shells`) | 1 failure — its own dedicated test (`test_paint_depth_clamp.cpp:684`, `paint_depth_solid_interfaces=false falls back to plain interface_shells`) |
| 2 | `PrintObject.cpp:1773` (`discover_vertical_shells`'s `top_bottom_surfaces_all_regions`) | 1 failure — its own isolation test (`:835`, `area_on > area_off`, now `area_on == area_off`) |
| 3 | `PerimeterGenerator.cpp:622` (Classic `split_top_surfaces`) | 1 failure — the NEW `len_on != len_off` pin (`:994`), `len_on == len_off` exactly (15629589160.0 both) |
| 4 | `PerimeterGenerator.cpp:2273` (Arachne `process_arachne`) | 1 failure — the NEW `len_on < len_off` pin (`:958`), `len_on == len_off` exactly (15634061952.0 both) |

Every mutation produced EXACTLY one failure, in its own dedicated test, no collateral failures.
`git diff --stat` and a `FIXWAVE2-MUTATION` marker grep on both production files are empty after
all four reverts.

---

## I-C — gap-fill-off widening resolved per layer, from real per-layer paint presence

**Bug found by my own testing, not merely reasoned about.** The review's suggested implementation
("build `claim_width_gapfill_off[layer][colour]` from regions with non-empty `slices`") does not
work when gated on `LayerRegion::slices` for the auto-created PAINTED PrintRegion itself: that
region's `slices` is EMPTY on every layer at this point in the pipeline (`apply_mm_segmentation`,
which writes it, runs AFTER `multi_material_segmentation_by_painting` entirely — the same N1 fact
`layer_color_stat`'s own per-colour block already sidesteps by never gating on it at all). A first
attempt gating on it made the per-layer data permanently empty everywhere, silently regressing the
existing object-wide `gap_infill_speed=0` sphere test (caught immediately by that pre-existing
regression pin — not a new failure I introduced unnoticed, one my own build+test loop surfaced
before it could ship).

**Fix, in `segmentation_by_painting`** (which has `segmented_regions` — the real per-layer lateral
paint presence — that its caller does not): for each layer, (a) is a gap-fill-disabled region
config Z-applicable at all — ANY region on the layer (any colour) with non-empty `slices` AND
`gap_infill_speed <= 0` (reliable because a modifier's own BASE-material sibling variant is always
ordinarily sliced, independent of paint); if so, (b) for each painted colour with `segmented_
regions[layer_idx][color_idx]` non-empty (genuinely painted here) and a nonzero object-wide
gap-fill-off width, that layer's entry gets the object-wide value. Documented, not hidden:
with MULTIPLE gap-off modifiers on different colours at non-overlapping Z ranges, a layer where
only one is Z-applicable still widens every colour with a gap-off region ANYWHERE on the object —
narrower than the fully object-wide bug (a layer with no gap-off region anywhere nearby correctly
gets nothing), not perfectly per-colour precise.

**Tested both ways**, on a new 3.47×40×10mm box (all sides Extruder2, keep-core = width − 2×band =
0.59865mm — inside the M3 thin-residue window) plus a `PARAMETER_MODIFIER` slab at z∈[9,10] with
`gap_infill_speed=0`:
- Does NOT over-widen at an unrelated layer (z≈2.6mm): RED confirmed (stash-based, see below),
  GREEN after.
- The "still widens at the modifier's own z" direction cannot be shown on THIS SAME probe: the
  centre residue there is the SAME keep_core component `cut_segmented_layers` already protects,
  and I-D's own fix means it is correctly exempted regardless of gap-fill state (by design — a
  keep-core component must never depend on gap-fill state to stay protected). Verified directly
  (stays base at z≈9.5mm too, for the DIFFERENT reason of I-D's protection) rather than assumed.
  The genuine "widens a real inter-claim sliver when gap fill is off" direction is exactly what
  the pre-existing `gap_infill_speed=0` sphere test already proves and continues to prove
  unmodified (a curved two-colour boundary has no keep_core component to protect it).

---

## I-D — keep-core re-test now uses the widened `t`

One-token fix at the re-test site (`merge_segmented_layers`): `const float t_keep_core = t;`
reusing the SAME per-island `t` (`scaled<float>(effective_claim_width * 0.5f)`, already computed
above for the outer absorb test, M4's own per-island gap-fill-off widening included) instead of
recomputing a narrower `scaled<float>(min_claim_width * 0.5f)`. Identical to the pre-fix value
whenever gap fill is on everywhere (`effective_claim_width == min_claim_width` there).

New fixture: `slice_painted_box(3.5, 40., 6., ALL_SIDE_FACE, pdmWalls, 3, 0., 0.1, print,
/*interlocking_depth=*/-1. [registry default, i.e. the notch stays live — unlike Minor 3's own
probe, which forces it to 0], Arachne, /*gap_infill_speed=*/0.0)` — width 3.5mm sits inside the
review's own measured churn window (3.42, 3.62]mm. RED confirmed (odd layer wrongly absorbed,
even layer presumably still base — the churn); GREEN after (both parities stay base).

---

## Minor — disjointness pin (geometrically-justified floor, not tuned) + corrected comment

**2×3 disjointness.** New test on the sphere fixture: for every layer, `intersection_ex(claim2,
claim3)` must stay below a cited (not invented) Clipper-rounding ceiling. Measured value: a single
5.2×10⁻⁹mm² fragment at one layer — about four to five orders of magnitude below the SAME
`0.0001mm²` ceiling `has_painted_unopened_fragment`'s own header comment already cites (itself
citing `has_interclaim_sliver`'s independently-measured "~430 sub-0.00002mm² Clipper fragments").
Using a raw `.empty()` check is what this file's OWN `opening_ex`-based tests elsewhere already
avoid, for the same reason: each colour runs its own independent chain of unions/diffs/offsets
(including `merge_segmented_layers`'s per-colour "Remove dimples (#7235)" `offset2_ex` close), so a
hairline mismatch at a shared boundary is expected arc-approximation noise. `0.0001mm²` is the
SAME externally-documented constant this file already uses elsewhere, with four orders of
magnitude of margin — not a floor invented to fit this specific residual.

**Comment correction.** The I1 fix site's comment claimed the new formula was "exactly the same
value as the old one whenever `legacy` is a subset of `full` (the invariant that held before Item
2)" — false: `legacy` is by design never priority-trimmed nor clipped by another colour's surface
contribution, while `full` is both, so the subset relationship never generally held, Item 2 or
not. Corrected to state the real argument (subtract-only: `diff_ex(full, X)` can only shrink or
preserve `full`, never add area back, regardless of what `legacy`/`excess` contain) and to note
this wave's own complementary `reach`-opening fix (previously "considered and rejected" in this
same comment as an alternative — now landed, as a complement, not a replacement).

**m3 (unit test SECTION 1).** `interclaim_absorb_effective_claim_width`'s "non-bordering colour"
SECTION left `claims[3]` empty, exiting through the `painted_claims[color_idx].empty()` shortcut
without ever exercising `intersection_ex(dilated, ...)`'s negative path. Now `claims[3]` is a
real, non-empty, far-away rectangle (`x∈[3000,4000]`, dilated island only reaches `x∈[-10,1010]`)
— genuinely exercises the adjacency test.

---

## RED evidence (real, stash-based — not simulated)

`git stash push -- src/libslic3r/MultiMaterialSegmentation.cpp` (production reverted to `8c5bf752de`,
every new/modified test left in place), rebuilt, ran `[paintdepth]`:

```
test cases:   87 |   84 passed | 3 failed
```

- `test_paint_depth_clamp.cpp:4204` — `CHECK_FALSE(has_painted_unopened_fragment(*object, 2))` →
  `!true` (I-A: the un-opened shadow leaks a raw fragment pre-fix).
- `test_paint_depth_clamp.cpp:4379` — `CHECK_FALSE(any_contains(..., odd_layer, ...))` → `!true`
  (I-D: the odd-layer keep-core is wrongly absorbed pre-fix, the churn).
- `test_paint_depth_clamp.cpp:4519` — `CHECK_FALSE(any_contains(..., probe_layer, ...))` → `!true`
  (I-C: the z≈2.6mm centre is wrongly absorbed by the unrelated z=9-10 modifier pre-fix).

`git stash pop` restored the fix; rebuild + full re-verification below confirms GREEN. I-B's own
RED evidence is the four mutation results above (each ungated site alone reproduces exactly one
failure).

---

## Process / gates — all on the FINAL committed-state binary

- Build-slot checked (`Get-Process cl,link,MSBuild`) before every build; the shared worktree's
  build directory was occupied by the sibling review agent's own build twice during this task —
  waited (background `until`-loop, no busy-polling) rather than racing it. All builds run in the
  foreground with generous timeouts once the slot was free.
- `[paintdepth]`: **87 cases / 1197 assertions, all green** (baseline 80/1153; +7 cases exactly
  matching the 7 new `TEST_CASE`s this wave adds — 2 for I-B, 2 for I-A, 1 Minor, 1 I-D, 1 I-C).
  Identical under default order and two `--order rand` seeds (1 and 2): 87/1197 all three runs,
  confirmed by a timing-stripped diff (byte-identical after sorting).
- `[chameleon]`: **133 cases / 605 assertions, exact, unchanged.**
- `ALL_BUILD`: **exit 0, zero error lines.**
- `spike/verify_paintdepth.sh` ×2 on the final binary: **17/17 both times**, `RESULT: ALL PASS`,
  exit 0 both times — unpainted byte-parity and determinism both hold (this whole feature is
  gated behind `is_mm_painted()`, never reached for unpainted objects).
- **TRUE full `libslic3r_tests` suite** (plain, no filters): **515 test cases | 513 passed | 2
  failed as expected**; **51276 assertions | 51274 passed | 2 failed as expected**; exit 0. The 2
  expected failures are the same pre-existing `[!shouldfail]`-tagged `test_mixed_filament.cpp`
  cases every prior wave in this feature has recorded (`:3483`, `:4429`). 508 → 515 is exactly
  this wave's +7 new cases.
- I-B's four mutations: production files (`PrintObject.cpp`, `PerimeterGenerator.cpp`) confirmed
  byte-identical to their pre-mutation state after every revert (`git diff --stat` empty, no
  `FIXWAVE2-MUTATION` marker left behind).

## Self-review (hand-walk, no subagents)

- Read the full diff of both touched files end to end (not just the sections I wrote — the
  surrounding context at every edit site).
- Grepped every added line for `if (false`, `TEMP`, `#if 0`, `printf`/`cout`/`cerr`, `DIAG`,
  `UNGATE`, `FIXWAVE2-MUTATION`, `XXX`, `FIXME`: one hit, a false positive (`DIAG` matching inside
  the prose word "Re-diagnosed" in a comment) — no actual debug residue.
- Verified variable/parameter renames (`_by_color` → `_by_layer_color` in `merge_segmented_
  layers`/`segmentation_by_painting`; the reverted object-wide name in `multi_material_
  segmentation_by_painting`) are internally consistent by grepping every occurrence across the
  file after the final edit, not just the sites I touched directly.
- Confirmed the two production-file mutations left zero diff via `git diff --stat` AND a
  content-marker grep, not diff-stat alone.

## Commit

One commit, `fix(paint-depth):` prefix, touching `src/libslic3r/MultiMaterialSegmentation.cpp`,
`tests/libslic3r/test_paint_depth_clamp.cpp`, `.superpowers/sdd/2026-08-31-paint-depth/
progress.md`, plus this report. Fixtures (mesh/STL assets, existing fixture-builder functions)
untouched in the sense that matters — every new test reuses an existing fixture-builder function,
extended with a new trailing, backward-compatible, default-preserving optional parameter (the
SAME convention this file has used in every prior wave — `gap_infill_speed`, `only_one_wall_top`,
`wall_generator` are all precedent), never a new fixture-builder function or mesh asset; one
genuinely new inline E2E construction for I-C's modifier-based fixture (no reusable builder
existed for a `PARAMETER_MODIFIER` scenario before this wave). Defaults untouched.
