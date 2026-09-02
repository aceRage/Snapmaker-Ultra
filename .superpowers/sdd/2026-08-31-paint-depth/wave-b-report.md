# Wave B — Option N, the normal-thickness paint shell

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, parent `ef9c20d90f` (Wave A).
Scope: `curved-gap-design.md`'s recommended **Option N** — the painted claim becomes a
constant-thickness shell measured NORMAL to the painted surface — plus its two named hazards and
the config-semantics change. Nothing from Wave A was re-litigated.

---

## 1. The numbers, measured

Reproduced by a new test that scans the claim outward from each layer's own contour in 0.05 mm
steps and converts to normal thickness (`reach · sin θ`). Square frustums tapering to an 18 mm top
over 3 mm, `paint_depth_mode = walls`, `paint_depth_walls = 3` (⇒ `D = 1.435675`), 0.45 mm outer
and inner walls, 0.1 mm layers, probe layer 12.

| θ | before (band·sinθ) | design predicted (D·cosθ) | **measured now** | reach measured |
|---|---|---|---|---|
| 10° | 0.249 | 1.414 | **1.476** | 8.50 mm |
| 15° | 0.372 | 1.387 | **1.436** | 5.55 mm |
| 20° | 0.491 | 1.349 | **1.402** | 4.10 mm |
| 25° | 0.607 | 1.301 | **0.549** (unchanged) | 1.30 mm |

**10–20° land ABOVE the design's figures** and that is correct, not luck: the descent depth is a
whole number of layers, `M = ceil(D/h) = 15`, so the delivered thickness is `M·h·cosθ = 1.5·cosθ`
— the design's `D·cosθ` rounded up by one layer of quantisation, never down. The 6.5° cliff is
gone: 6.49⁻ and 6.49⁺ now both give ≈`D·cosθ`.

**25° is the one number the design got wrong, and it is a real ceiling — see §2.1.** Everything at
25° and above is byte-identical to before this wave.

Gains actually delivered: **5.9× at 10°, 3.9× at 15°, 2.9× at 20°, 1.0× at 25°.**

---

## 2. Findings the design did not have

### 2.1 The opening filter caps Option N at 23.96°, not at 58.5° — HONEST LIMIT

`segmentation_top_and_bottom_layers` runs `top_ex = opening_ex(top_ex, small_region_threshold)`
*before* the descent begins. `small_region_threshold` is `scaled(0.5 · 0.5 · outer_wall_line_width)`
= 0.1125 mm at a 0.45 mm outer wall with gap fill on, so a staircase ring narrower than
**0.225 mm** is erased outright and there is no vertical claim left for Option N to deepen. The
ring is `r = h/tanθ` wide, so the vertical half of the claim exists only for

> **θ < atan(layer_height / 0.225)** = **23.96°** at 0.1 mm layers, 41.63° at 0.2 mm.

The design's own §1 records this filter (its third regime, "> ~23.96°") and its §3-Option-B
analysis even notes `r = 0.215` at 25° is below the threshold — but Option N's headline table
nonetheless extends `D·max(cos, sin)` to 30/45/60°. It does not hold there.

The design's derived F1 self-suppression at 58.5° is therefore **never the binding constraint at
any practical layer height**: F1 suppresses when `r < 0.627·h`, the opening when `r < 0.225`, and
`0.627·h < 0.225` for all `h < 0.359 mm`. The steep-slope test (T3) consequently pins the
*suppression* and names which guard delivers it, rather than claiming F1 did it.

**Consequence for the user.** Their stated problem — features on a domed face that the setting
was "inert below ~6.5° and thin between 6.5–27°" — is fixed across 0–24°, which is where the
shallow end of a dome lives. Between 24° and ~45° the claim is still `D·sinθ` (0.58 mm at 24°,
1.02 mm at 45°) and the discontinuity has moved from 6.5° (a 3.7× cliff) to 24° (a 2.3× cliff).
Closing that too means lowering the #7104 sliver guard, which the design examined as its Option B
and rejected on sliver risk. **Not done here.** It is the obvious next wave if the user reports a
visible seam at ~24° on a dome; the measurement test above is already written to fail loudly (with
an explanation) if that threshold ever moves.

### 2.2 The base filament must NOT get the paint's normal thickness — CAUGHT BY AN EXISTING TEST

The first Wave B build deepened *every* colour's descent, including `color_idx == 0`. That
inverted the feature, and the committed I-3 test caught it (2 assertions, its 6 mm band collapsed
to 0.857 mm).

Mechanism: `merge_segmented_layers` trims **every** extruder's *lateral* claim by **every**
extruder's *top/bottom* claim (`diff_ex` over `top_and_bottom_layers`) before appending its own.
So a base-colour claim descending `D` deep from an **unpainted** top cap cuts the painted lateral
band back to one wall stack on every layer beneath that cap. On a 40×40×6 mm box with one painted
side and `paint_depth_mm = 6`, the 6 mm band became 0.857 mm.

The rule is now explicit in code and in a dedicated named test: **paint depth bounds paint.**
`color_idx == 0` is not a paint claim — its top/bottom claim exists solely to stop a neighbouring
painted colour smearing across the solid shell under an unpainted cap, and that contract is
written in shell terms (`top_shell_layers` / `top_shell_thickness`). All of N1, N2 and N3 are
gated on `color_idx > 0`, so the base colour's descent is byte-identical to pre-Wave-B.

### 2.3 Widening `granularity` does not by itself close the TBB race — it needed the alignment fix too

See §3.4. The design's hazard 2 is real, but its stated remedy is necessary and **not sufficient**.

---

## 3. What changed

Five files, one commit.

### 3.1 N1 — the `exposed_surface_part` slope gate is retired on the painted path

`MultiMaterialSegmentation.cpp`, both descent loops. Where the normal-thickness shell is active
the full-width term is simply `top_ex` (resp. `bottom_ex`); where it is not, the call is
unchanged and the behaviour is byte-identical.

The gate was a **proxy** ("reject steep surfaces") applied to a whole patch and measured against
the *neighbouring* layer; F1's `offset_ex(input_expolygons[last_idx], -wall_stack)` enforces the
**actual** invariant pointwise on the layer the claim lands on. Two mechanisms for one invariant,
and the proxy's only remaining effect was to throw away a claim the descent had already computed
correctly. `exposed_surface_part()` itself is untouched and still used on the legacy path; its
header now says so.

### 3.2 N2 — the descent is bounded by normal depth `D`, not by the shell layer count

New `LayerColorStat::top_descent_layers` / `bottom_descent_layers`, kept **separate** from
`top_shell_layers` / `bottom_shell_layers` because the "is anything claimed at all" gates are C1
contracts about the *shell* (a zero shell count claims nothing, not even the painted surface
facet) and must not be deepened. The bound is

    descent = shell > 0 ? max(shell, effective_shell_layers_by_thickness(layers, k, top, 1, D)) : 0

Reusing `effective_shell_layers_by_thickness` with a layer count of 1 means the thickness walk runs
against this object's **real** `print_z`/`bottom_z` values, so variable layer height is exact and
the `< thickness - EPSILON` boundary matches the solid-shell generators — the design's §6 hazard 5,
closed by reuse rather than by a `D / layer_height` division.

`max(shell, …)` preserves the earlier shell-coverage wave's contract when `D < shell`.

### 3.3 N3 — the `break` is relaxed, with an early-out that keeps it terminating cheaply

    if (last.empty()) { if (normal_shell && ! deposited) continue; break; }

On a slope the full-width term is empty for the **near** steps: the ring deposited `m` layers down
sits at inset `[m·r, (m+1)·r]` and F1 holds it one wall stack clear, so nothing survives until
`(m+1)·r > wall_stack` — `m ≥ 2` at 15°/0.1 mm. The legacy eroded term is empty there too. The old
eager `break` fired at step 1 and the entire change would have been a silent no-op.

Two deliberate details:

* the flag is **`deposited`** (post-`opening_ex`), not "a non-empty raw term was seen". At 15° the
  first surviving strip is `[0.87854, 1.1196]` — 0.2411 mm against the 0.225 mm printability
  threshold, a 16 µm margin. Keying on the raw term would let Clipper's arc approximation truncate
  the whole descent one step early on a knife-edge;
* `reachable = top_ex ∩ layer_slices_trimmed` is computed once and, under the extension, an empty
  `reachable` **breaks** — `layer_slices_trimmed` only shrinks and `top_exposed_ex` is fixed, so
  nothing at this depth or below can be claimed by either term. That is what stops a tall object
  paying `M-1` empty Clipper rounds once the descent has run off its own geometry.

### 3.4 N4 — `granularity`, and the alignment bug underneath it

**Both halves were needed.**

*Half one (the design's).* `granularity` is the correctness margin of the TBB double-buffer parity
trick, not a perf knob: a layer reaches back `descent-1` layers into
`shell_triangles_by_color_*[last_idx + layer_idx_offset]`, and that must stay inside the previous
group, which has the opposite parity. It was sized from *shell* counts (5 at defaults) while the
descent is now 15 deep. Now sized from the same `*_descent_eff` the loop bound uses.
`max_top_layers` / `max_bottom_layers` are widened identically; they are only used as
"is there a shell at all" booleans and `*_descent_eff > 0 ⟺ *_layers_eff > 0`, so C1 is untouched.

*Half two (found while verifying half one).* The trick **also** assumes each TBB chunk begins on a
multiple of `granularity`. `tbb::blocked_range` splits at **midpoints** until a chunk is no larger
than the grainsize, so chunks come out sized in `(G/2, G]` starting wherever the halving lands.
`blocked_range(0, 32, 14)` yields `[0,8) [8,16) [16,24) [24,32)` and `range.begin() / granularity`
labels the first **two** as group 0 — adjacent, same parity, so layer 8's descent writes into slots
0–7 of the buffer the `[0,8)` chunk is concurrently writing. **A data race, and pre-existing**: at
the old grainsize of 5 the chunks come out size 4 and collide identically. Widening the write-back
distance from 5 layers to 15 makes it far likelier to bite, so this wave closes it rather than
inheriting it.

Fix: iterate the **groups** (`tbb::parallel_for(size_t(0), num_groups, …)`, the index form already
used in `SL1.cpp` and `SlicesToTriangleMesh.cpp`), so a group's layer range is
`[g·G, (g+1)·G)` by construction. TBB may still hand several *consecutive* groups to one task —
sequential within a task, therefore safe — and the invariant holds unconditionally: a layer reaches
back at most `G`, i.e. never past the previous group, which always has the opposite parity. Same
per-layer body, same work, same partition sizes; only the boundaries move. No re-indentation (the
index form keeps the identical nesting depth).

### 3.5 Config semantics

`PrintConfig.cpp`, three tooltips. **No default VALUES changed** (standing decision):
`paint_depth_mode = walls`, `paint_depth_walls = 3`, `paint_depth_mm = 1.5`.

* **`paint_depth_mm` is now the headline control**, redefined as normal thickness: "how thick a
  painted claim is, measured perpendicular to the painted surface … the same everywhere". The
  user's ~2 mm target is a direct entry, `paint_depth_mm = 2.0`, with no `wall_loops` prerequisite.
* **`paint_depth_mode`** describes the depth as a perpendicular thickness and names "Limited by
  distance" as the control to reach for.
* **`paint_depth_walls`** keeps Wave A's number (`D = 1.435675` at N = 3) and drops the loop-count
  promise: "about that many wall widths of material measured perpendicular to the painted surface
  … a thickness, not a promise of a loop count". The honest reason is in `PaintDepth.hpp`: F3's
  formula is shaped for Arachne's bead-count windows and carries no meaning under the **classic**
  generator, where `process_classic` yields two external-width loops plus one gap-fill line for any
  band in the 1.3–1.5 mm range regardless of N — and the user runs classic. The number is kept
  rather than switched to the generator-neutral `ext_w + (N-1)·s = 1.30708` because it preserves
  F3's real Arachne count-margin win on vertical walls, is only ~10 % above that reading, and
  avoids a second band formula.

`PaintDepth.hpp`'s header for `paint_depth_band_mm` now states that the one number bounds **both**
halves of the claim, with the derivation and the 23.96° limit.

### 3.6 The `D ≥ wall_stack` gate, and how it meets Wave A's classic floor

`normal_shell` is true only when the colour is painted **and**
`scaled(D) + SCALED_EPSILON ≥ extrusion_spacing + extrusion_width`. Below one wall stack the
lateral band reaches only `D` while the F1-inset descent starts at `wall_stack`, leaving the base
region a closed ring of width `wall_stack − D` sandwiched between two painted annuli on every
sub-surface layer — a new sliver class.

**Interaction with Wave A, stated as asked.** At `paint_depth_walls = 1` the two generators land on
opposite sides of the gate, and it is Wave A's classic floor that puts them there:

| generator | band(1) | vs `wall_stack` = 0.878540 | extension | sandwiched ring |
|---|---|---|---|---|
| classic | `max(0.578595, ext_w + ext_s)` = **0.878540** | **equal** | **on** | **zero width by construction** |
| Arachne | 0.578595 | below | off | n/a (legacy behaviour) |

Wave A floored the classic band precisely because `band < wall_stack` left the base region a void
ring under every painted cap; that floor makes `band(1)` and the F1 inset *the same number*, which
is exactly the condition this gate needs. The two fixes meet at the same value from opposite
directions. The `SCALED_EPSILON` slack exists because of that equality: `D` and `wall_stack` reach
the gate down different float paths and a rounding ULP must not decide whether a user gets a
normal-thickness shell. Both sides are pinned by a two-section test.

---

## 4. Tests

**RED first, on the shipped tree + tests only.** `48 cases, 647 assertions | 3 cases / 8 assertions
failed` — and exactly the predicted eight:

| case | failing assertions | reading |
|---|---|---|
| T1 | probes at 3.0 and 5.0 mm | claim stopped at the 1.435675 mm band |
| break placement | probes at 1.6 / 2.5 / 3.5 / 4.5 / 5.0 mm | every descent step beyond the band absent |
| classic `walls=1` gate | probe at 3.0 mm | ditto, on the floored classic band |

T2 (F1 pin), T3 (steep suppression) and the Arachne `walls=1` section passed in the RED run, as the
design predicted — they are regression pins, not RED targets.

Seven new test cases (`[paintdepth]`):

1. **T1 — shallow slope reaches normal depth.** 15° frustum. Claimed at 3.0 and 5.0 mm; **not** at
   6.0 mm, so it pins a *depth*, not "more paint": reach bracketed in [5.0, 6.0] ⇒ normal thickness
   bracketed in [1.294, 1.553] around `D = 1.436` / `M·h·cos = 1.449`.
2. **Break placement.** A graded ladder at 1.6 / 2.5 / 3.5 / 4.5 / 5.0 mm, each in the slot of a
   different descent step (m = 4, 6, 9, 12, 13) and all beyond the lateral band. A misplaced break
   fails at the first step it truncates and the message names the depth.
3. **T2 — F1's no-exterior-bleed invariant at the new depth.** Cap-only-painted 15° frustum, so
   there is no lateral band anywhere to mask a regression. Exterior 0.3 mm base at depths 1–5
   (margins ≥ 0.58 mm); 1.5 mm claimed at depths 1–3 (margins 0.62 / 0.57 / 0.19 mm), so the
   invariant cannot be satisfied by claiming nothing. Green before **and** after.
4. **T3 — 63.4° suppression.** Lateral band claimed at 1.0 mm, nothing at 2.0 mm. Documents that
   the opening filter, not F1, is what fires here.
5. **Normal thickness across slopes.** The §1 table, executable, with the 25° case asserted as
   band-only and commented with why.
6. **Base filament is not given the paint's normal thickness.** §2.2, pinned by name.
7. **The `D ≥ wall_stack` gate** — classic (on, reach 3.0 mm claimed / 4.0 mm not) and Arachne
   (off, 0.3 mm claimed / 3.0 mm not), both at `walls = 1`. This is the **new classic-generator
   coverage** for the new semantics: the floor is what decides the gate.

**The committed anti-smear tripwire is UNMODIFIED and green.** `git diff --numstat` on the test
file shows **zero deletions**: no existing test was edited at all, only appended after. The
tripwire stays green for a stronger reason than the design gave: its `pdmMillimeters 0.15` is below
`wall_stack` (0.8357 at 0.3 mm layers), so the `D ≥ wall_stack` gate keeps it entirely on the
legacy path — a mode-and-depth gate, not merely `M = 1`.

---

## 5. Gates

All re-run on the final binary (`libslic3r_tests.exe` 08:56:13, newer than every source file in the
commit) after the final `ALL_BUILD`.

| gate | result |
|---|---|
| `[paintdepth]` | **682 assertions in 50 test cases, all pass** (baseline 579 / 43) |
| `[chameleon]` | **605 assertions in 133 test cases, all pass** — exactly at baseline |
| full `libslic3r_tests` | 478 cases, 476 passed, 2 failed-as-expected, **exit 0** |
| `ALL_BUILD` (scratchpad `build_next_wt.bat`) | **exit 0**, zero errors |
| `spike/verify_paintdepth.sh` ×2 | **17/17 ALL PASS** both runs |

`verify_paintdepth.sh` includes `unpainted-run{1,2}-vs-baseline` byte-identical (normalized) against
the frozen pre-feature baseline and `unpainted-determinism` — the parity that matters most here,
because this wave touches a TBB partition. It holds: the whole change is paint-gated, and
`segmentation_top_and_bottom_layers` is only reached for MM-painted objects.

**One accounting note on the full suite.** With `--warn NoAssertions` added, two extra test cases
are reported failed: `Hollow two overlapping spheres` and `Voronoi missing edges - points 12067`.
Both contain no assertions at all, which is what that flag reports; both are present and equally
assertion-free in Wave A's own full-suite log (`wavea_green_full.log`), which simply did not pass
the flag. Without it the run is exit 0 as above. The two genuine `FAILED` assertion lines
(`test_mixed_filament.cpp:3469` and `:4415`, both in test cases whose own names say
"(KNOWN bug)" / "(differential)") are byte-for-byte the same two Wave A recorded — pre-existing,
`[!shouldfail]`-tagged, and untouched by this wave. Case count 471 → 478 is exactly the 7 cases
added here.

---

## 6. Cost, stated plainly

**Fix-wave correction (wave-b-review.md Important 3):** the paragraph below originally read
"Material: none wasted... total extrusion is unchanged" — false, and self-contradicted by the
very next paragraph, which correctly says each newly painted layer costs a tool change and a
purge. Corrected headline: **the claim volume is a re-colouring, not extra solid — but the job's
total extrusion goes UP**, by two independent mechanisms neither of which is a shift:

1. **Purge / prime tower.** 6 → 15 painted layers on a flat cap is **9 extra tool changes**.
   `flush_volumes_matrix`'s default in this tree is **280 mm³ per change** (`PrintConfig.cpp:6519`),
   so ≈ **2520 mm³ ≈ 2.5 cm³ ≈ ~3 g of PLA** of purged filament added per painted flat cap, at
   stock defaults — on a small painted part that can exceed the object's own volume. Purge is
   extruded material, not a shift.
2. **Colour-boundary wall loops.** Every newly split layer (the 9 extra layers between the old
   6-layer solid shell and the new 15-layer normal-thickness claim, which are sparse-infill
   layers at stock `top_shell_layers`/`top_shell_thickness`) gains a full set of wall loops around
   the new colour boundary — dense extrusion replacing ~15%-density infill. Net material up.

The "none wasted" intuition is true, but only of the object's own solid volume at constant
infill density (`paint_infill_override` defaults to `true`, so the painted claim's sparse infill
stays sparse and merely changes colour) — a strictly narrower statement than the one originally
made here. See curved-gap-design.md §7 for the same correction at its source.

**Positive result, stronger than this report's own "byte-identical" framing elsewhere (review's
Minor 1):** on the painted path, Wave B's claim is a strict SUPERSET of the legacy claim at every
slope, so nothing anywhere is degraded — provable independent of slope purity, unlike "24°+ is
byte-identical" which needs a pure slope to hold.

**Tool changes / purge — the real cost, and the user has accepted it per the standing fidelity
ruling.** On a painted **flat top** the number of layers carrying a painted region goes from the
solid-shell depth to the normal-thickness depth: **6 → 15 layers at 0.1 mm** (4 → 8 at 0.2 mm) at
stock defaults. Each newly painted layer costs one extra tool change and its purge. On a **slope**
the layer count does not change (the lateral band already put paint on every sloped layer); only
the region gets wider. So the cost is concentrated on flat and near-flat painted faces, and it is
the direct consequence of the constant-thickness semantics that were asked for. The dial is
`paint_depth_mm`.

**Lateral footprint grows a lot on shallow slopes** — measured 8.50 mm at 10°, 5.55 mm at 15°,
4.10 mm at 20°. That is correct (the normal thickness is still ~1.44 mm) but it is visually
striking in preview and should be expected, not filed as a bug.

**Slice time.** The descent no longer breaks at step 1 on a slope: up to 14 Clipper offset+intersect
pairs per painted surface layer instead of 1, each on `top_ex ∩ layer` (small), TBB-parallel. The
`reachable.empty()` early-out caps the waste once the descent runs off the geometry. The paint-depth
stage should be expected to dominate slice cost on organic models.

---

## 7. Concerns / residue

1. **The 23.96° ceiling (§2.1).** The single most important thing to relay: the dead band is closed
   from 0° to 24°, not to 45°. The design doc's Option N table should be read with §2.1 alongside.
2. **No GUI slice was run.** Every number here is from `slice()`-level tests and hand arithmetic.
   The visual consequences — a 8.5 mm-wide painted footprint at 10°, and 15 painted layers on a
   flat cap — have not been eyeballed.
3. **The TBB alignment fix (§3.4) touches an upstream parallel loop.** It is provably equivalent
   per-layer and the whole suite is green, but it is the one change in this wave whose blast radius
   is wider than paint depth. It is inert for unpainted objects (this function is only reached for
   MM-painted ones) — the byte-parity gate covers that.
4. **`granularity` can now approach `num_layers`** in millimetres mode at a large depth, which
   serialises the top/bottom stage for short objects. Correct, but a throughput loss on e.g. a
   30-layer object at `paint_depth_mm = 6`.
5. **The `deposited` flag means a painted colour never breaks early on a slope it cannot reach.**
   Bounded by the loop and by the `reachable` early-out, but on a shape where `reachable` stays
   non-empty and no step ever deposits (a steep wall whose ring survives the opening — needs
   `h > 0.359 mm`), the loop runs all `M-1` steps doing empty Clipper work. Correct, not free.
6. **Wave A residue carried forward unchanged**: F2's Voronoi wrap-around at a thin fin tip is still
   not addressed (the design's Option C footnote — a painted-seed ball dilation — remains the
   eventual answer), and Arachne's corner bead counts are still recorded rather than pinned.
7. **`paint_infill_override` matters much more now.** A flat cap's claim is 15 layers deep at
   defaults, well past the solid shell, so more of it lands in sparse infill. The option already
   governs that, but its practical weight has grown.
