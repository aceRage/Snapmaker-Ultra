# Curved-surface opacity gap — design options for the next change

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `193d2e0675`.
Read-only design pass; **no source edited**. Inputs: `taper-bound-review.md` (check 5),
`wall-count-investigation.md` (§6), `outward-bleed-investigation.md`,
`bleed-and-walls-fixwave-report.md` (§8 flags), plus a fresh read of
`MultiMaterialSegmentation.cpp`, `PaintDepth.{hpp,cpp}`, `PerimeterGenerator.cpp`,
`PrintObject.cpp`, `Layer.cpp` and `tests/libslic3r/test_paint_depth_clamp.cpp`.

Marks: **[V]** verified by reading committed code (file:line given); **[D]** arithmetic
derived from that code. No build or slice was run by this pass.

---

## 0. Mid-design reframing from the user — and what it changes

The user's stated ideal, relayed mid-task:

> painted colour regions should behave like **separate objects** (as split-by-colour tools
> produce) but **without the tolerance gap** — "a certain amount of inward taper and normal
> projection so the coloured section becomes its own object and then is printed how a
> separate object would print."

Read as engineering intent that is a single rule: **the painted claim is a constant-thickness
shell measured NORMAL to the painted surface**, not a lateral band and not a vertical layer
count. For a surface at slope θ from horizontal, a normal thickness `D` implies

    lateral extent  = D / sin θ        (degenerates to the current band at θ = 90°)
    vertical extent = D / cos θ        (degenerates to a layer-count shell at θ = 0°)

and it covers the 6.5°–27° dead band continuously in between, because it never had a dead
band — the dead band is an artefact of using *distance-to-contour* (a 2D proxy) and
*layer count* (a 1D proxy) as two disjoint approximations of one 3D quantity.

**This is candidate (c) generalised, and the design below adopts it as the recommendation.**
The good news, derived in §2, is that the normal-thickness rule is **already latent in the
existing descent loop** — the top/bottom projection path computes exactly the right thing and
is switched off by two guards. So the user's ideal is reachable by a *bound change plus a
guard removal*, not by a new mechanism.

---

## 1. The problem restated with current-HEAD numbers

Three regimes, all **[V]**:

| regime | mechanism | why it stops |
|---|---|---|
| θ < 6.49° (0.1 mm) / 13.1° (0.2 mm) | full-width vertical descent, `:1715` loop with the F1 term at `~:1780` | `exposed_surface_part()` (`:1390`) returns empty once the per-layer staircase run `r = h/tanθ` drops below one wall stack `w+s` |
| 6.5° … ~24° | lateral band only (`cut_segmented_layers` `:1210`, core at `:1240`, cut at `:1244`) | nothing stops it — it is simply thin: `t = b·sin θ` |
| > ~23.96° (0.1 mm) | lateral band only | `opening_ex(top_ex, small_region_threshold)` (`:1687`, bottom twin `:1794`) deletes strips narrower than 0.225 mm |

The 6.5° gate and the ~24° opening are **not** the two ends of one gap — they are two
*independent* switch-offs of the *vertical* contributor. The whole 6.5°–90° range is served
by the lateral band alone, and the band's normal thickness is `b·sin θ`.

**[D] Recomputed with the F3 band** (`PaintDepth.cpp:9-29`; walls = 3, 0.45 mm outer/inner
line, 0.4 nozzle, 0.1 mm layers ⇒ `s = s_ext = 0.428540`, `band = 1.435675`,
`wall_stack = w+s = 0.878540`). Note check 5's table used the pre-F3 band of 1.307:

| θ | run `r = h/tanθ` | today `t = band·sinθ` | below-6.5° reference |
|---|---|---|---|
| 6.49⁻ | 0.8785 | — | **0.596** (shell 0.6 × cos θ) |
| 6.49⁺ | 0.8785 | **0.1625** | — (3.7× cliff) |
| 10° | 0.5671 | **0.2493** | |
| 15° | 0.3732 | **0.3716** | |
| 20° | 0.2747 | **0.4910** | |
| 25° | 0.2145 | **0.6067** | |
| parity with 0.60 mm | | at **24.70°** | |

So with F3 the gap is 6.5°–24.7°, worst at the shallow end, and the 6.5° cliff is a factor
of 3.7 rather than 4.0. F3 helped ~10 %; the gap is otherwise unchanged.

---

## 2. The key derivation — the descent loop already computes the normal shell

Let the local silhouette have slope θ, layer height `h`, so consecutive layer contours step
inward by `r = h/tan θ`.

**[D] Lateral claim.** `cut_segmented_layers` keeps material within `b` of the layer contour.
In the vertical cross-section containing the surface normal `n = (sin θ, cos θ)`, the deepest
kept point is `b` horizontally in, at normal distance `b·sin θ`. (Reproduces check 5.)

**[D] Vertical claim.** `top_raw[j]` after the occlusion trim (`:1568-1570`) is the exposed
staircase step of layer `j` — the annulus between contour(j) and contour(j+1), width `r`. The
descent from surface layer `j` deposits on layer `k = j−m` a claim occupying lateral inset
`[m·r, (m+1)·r]` measured from contour(k). Union over the surface layers that can reach layer
`k` (`j = k … k+M−1`, i.e. total depth `M`) gives lateral inset `[0, M·r]`, hence

> **normal thickness of the vertical claim = M·r·sin θ = M·h·cos θ.**

**[D] The same statement from the "inward normal sweep" side.** Sweep the painted surface
inward along `−n` by distance `s ∈ [0,D]`. Parameterising the silhouette as
`x = S_j − (z−z_j)·cot θ` and solving for the points that land on layer `k = j−m`:

    x = S(z_k) − s/sin θ           for every m

i.e. the sweep's trace on **any** layer is the band `[0, D/sin θ]` from that layer's own
contour, independent of `m`. The prism sweep and the layer-count descent are the *same set*.

**Consequences that decide the design:**

1. The descent path is **inherently slope-correct**. It needs no angle measurement, no
   pointwise slope estimate, no variable-radius offset — the staircase geometry carries the
   slope for free. `M·h·cos θ` is exactly the "shell thickness projected normal to the
   surface" answer.
2. Constant normal thickness `D` therefore needs `M(θ) = D/(h·cos θ)` layers of descent.
   Since `cos θ ∈ [0.707, 1]` for θ ≤ 45°, a **fixed** `M = ceil(D/h)` delivers `D·cos θ`,
   and the lateral band delivers `D·sin θ` on the same geometry. Their union is
   `D·max(cos θ, sin θ)` ∈ **[0.707 D, D]** for *every* slope, worst at 45°.
3. The 6.5° gate (`exposed_surface_part`) is the only thing preventing this, and F1's
   `offset_ex(input_expolygons[last_idx], −wall_stack)` term (`~:1780`) is a **strictly
   better** version of the same protection (§5).

---

## 3. Options

### Option A — apply the lateral clamp to top/bottom claims too (candidate (a))

**Mechanism.** Either reorder `:2591` / `:2598` / `:2602` so `cut_segmented_layers` runs on
the merged result, or call `paint_depth_clamp_keep_core()` on `top_and_bottom_layers` before
`merge_segmented_layers`.

**What it does across 6.5–27°.** Nothing good. It replaces `t = max(vertical, band·sinθ)`
with `t = min(that, band·sinθ)` ⇒ **10°: 0.249, 15°: 0.372, 20°: 0.491, 25°: 0.607** — i.e.
today's numbers, with the *sub*-6.5° regime destroyed as collateral: a painted flat top's
claim is the whole cap, and clamping it to within `band` of the contour turns it into a
1.44 mm **ring** with base-coloured material in the middle. That is the feature's primary
working case.

**Why the premise is wrong.** The band is a *lateral* depth bound; the shell count is a
*vertical* depth bound. They are two projections of one 3D quantity, not two bounds on the
same axis. "The vertical path bypasses the clamp" is not a bug to close — it is the only
reason flat tops work at all. Closing it without replacing the depth semantics is a pure loss.

**F1 / slivers / cost.** Would create sub-wall-width painted rings on every painted cap
(exactly #7104). Reject.

**Verdict: reject.** It becomes *unnecessary* (not merely bad) under Option N, because there
the two bounds are unified into one `D`, so nothing "bypasses" anything.

---

### Option B — lower or remove `small_region_threshold` for top/bottom strips (candidate (b))

**Mechanism.** `:1687` `top_ex = opening_ex(top_ex, stat.small_region_threshold)` and the
bottom twin `:1794`; threshold `= scaled(0.5 · 0.5 · outer_wall_line_width)` = 0.1125 mm
(`:1663-1668`, gap-fill branch, `gap_infill_speed` default 30 > 0), deleting strips narrower
than 0.225 mm. Also `:1784` / `:1834` on the descended `last`.

**What it fixes across 6.5–27°: nothing.** **[D]** The opening only binds where `r < 0.225`,
i.e. θ > 23.96° at 0.1 mm layers (41.6° at 0.2 mm). Inside the gap the run is
0.567 / 0.373 / 0.275 / 0.215 mm at 10 / 15 / 20 / 25° — all comfortably above 0.225 except
right at the top end. Resulting normal thickness at 10/15/20/25° is **unchanged**
(0.249 / 0.372 / 0.491 / 0.607), because what it would restore is a *one-layer* strip of
normal thickness `h·cos θ ≈ 0.09 mm`, inside a region the lateral band already covers to
≥ 0.585 mm.

**Sliver risk: high, for no gain.** That opening *is* the #7104 guard the file names at
`:1354`. Removing it re-arms the exact class F1 spent a wave containing.

**Verdict: reject as the fix.** Keep as a possible later refinement only if a GUI slice shows
a missing outermost painted strip above ~24° that the lateral band demonstrably does not
cover.

---

### Option C — slope-aware lateral band, `b_eff = D/sin θ` (candidate (c), *lateral* realisation)

**Mechanism.** Inside `cut_segmented_layers`, replace the scalar `region_cut_width` with a
pointwise band. Segmentation is 2D, so the slope must be recovered from the slices: the local
staircase run is the width of `diff_ex(input_expolygons[k], input_expolygons[k±1])`, and
`r ↔ θ` by `tan θ = h/r`. Since Clipper has no variable-radius offset, this needs a **slope
ladder** in the style of F2's halving ladder (`paint_depth_clamp_keep_core`, `~:1188`):

```
step_up = diff_ex(layer[k], layer[k+1])                    // exposed up-facing staircase
for each ladder class i (r_i increasing):
    class_i = opening_ex(step_up, r_i/2) \ opening_ex(step_up, r_{i+1}/2)
    b_i     = min(D / sin(atan(h/r_i)), b_max)
    claim  ∪= intersection_ex(layer[k], offset_ex(class_i, +b_i))
… and the mirror for step_down = diff_ex(layer[k], layer[k−1])
```

**Numbers.** By construction `t = D` at every θ where the ladder resolves: **10°: D,
15°: D, 20°: D, 25°: D** (1.436 mm at walls = 3), capped to `b_max·sin θ` below the cap angle.
Geometrically the right answer.

**Three problems, one of them fatal:**

1. **F2 defeats it on exactly the target geometry.** **[V]** `paint_depth_clamp_keep_core`
   caps any lateral band at `b ≤ t/2` where `t` is the local half-thickness
   (fix-wave report §2, §8.4). A widened band on a locally thin curved shell — the organic
   case — is capped and the slope-awareness evaporates. The vertical path is not subject to
   F2 at all (it merges after the cut at `:2598`/`:2602`).
2. **The clamp is colour-agnostic.** `keep_core` is subtracted from *every* extruder's claim
   (`:1242-1244`). Widening the band on a slope lets **all** colours, including a neighbouring
   painted colour and the base, reach `D/sin θ` deep there — over-claim on multi-colour models,
   and it worsens the "Voronoi cell wraps onto the opposite face" residue the fix-wave report
   recorded as unresolved (§2, "needs a clamp measured from the PAINTED boundary").
3. It re-derives from 2D contours what the descent already knows exactly (§2), and adds a
   second mechanism to maintain alongside F2's ladder.

**Verdict: right semantics, wrong mechanism.** Adopt the semantics (Option N), implement them
through the descent.

*Note for the record:* the "measure from the painted boundary" variant — intersect each
colour's claim with `⋃_m offset_ex(painted_seed[k+m], +√(D²−(m·h)²))`, a Z-stacked ball
dilation of the painted surface trace — is the fully general 3D form and *would* also fix
residue (2) above. It costs `2·D/h + 1` offsets per layer (31 at defaults) and is a much
larger change. Recorded as the long-term target, not proposed now.

---

### Option D — unlock the vertical descent on slopes, shell-bounded (candidate (d))

**Mechanism.** Delete the `exposed_surface_part` gate at `:1711` / `:1813` (the `if
(! top_exposed_ex.empty())` guard at `~:1721`), keep F1's inset verbatim, relax the `break`
at `:1785` / `:1835` (see §6 hazard 1). Descent bound left at `stat.top_shell_layers`
(`:1717`), i.e. `S = 6` at defaults/0.1 mm.

**Numbers** `t = max(S·h·cos θ, band·sin θ)` = `max(0.6·cos θ, 1.4357·sin θ)`:

| θ | 10° | 15° | 20° | 25° |
|---|---|---|---|---|
| **t (mm)** | **0.591** | **0.580** | **0.564** | **0.607** |
| vs today | 0.249 | 0.372 | 0.491 | 0.607 |

Restores exact parity with the flat-top case (0.6 × cos θ) across the whole gap and removes
the 6.5° cliff. Cheapest possible change; **no extra painted layers on flat tops**, so no
extra tool changes there.

**Downside:** it delivers `shell·cos θ`, not constant `D` — i.e. it fixes the *cliff* but
leaves paint depth on shallow slopes governed by `top_shell_layers`, which is not what the
user asked for, and it **breaks the existing anti-smear test** (§7, hazard 3): that fixture
is 33.7° at 0.3 mm layers, `S·r = 4×0.45 = 1.80 mm > wall_stack 0.836`, so its 1.0 mm negative
probe would start passing. Under Option N the same fixture stays green untouched.

**Verdict: the fallback.** Take it if the tool-change cost of Option N proves unacceptable.

---

### Option N — **RECOMMENDED**: normal-thickness shell via a `D`-bounded descent

The user's ideal, implemented through Option D's mechanism with one extra change: **bound the
descent by normal depth `D` instead of by shell layer count.**

**Mechanism (four edits, all in `MultiMaterialSegmentation.cpp` + one in `PaintDepth.*`):**

| # | edit | anchor |
|---|---|---|
| N1 | delete the `exposed_surface_part()` slope gate; the full-width term becomes `top_ex ∩ layer_slices_trimmed ∩ offset_ex(input_expolygons[last_idx], −wall_stack)` unconditionally | `:1390`, `:1711`, `~:1721`, `:1813` |
| N2 | descent bound `stat.top_shell_layers` → `max(stat.top_shell_layers, layers_within_thickness(D))`, using the existing `print_z`-walk idiom so variable layer height is handled | `:1717`, `:1817`; pattern at `:1290-1338` |
| N3 | relax the early `break` so the descent is not terminated by the *near* steps being empty (see §6 hazard 1) | `:1785`, `:1835` |
| N4 | plumb `D` (the band already computed at `:2653`) into `segmentation_top_and_bottom_layers`, and widen `max_top_layers` / `max_bottom_layers` / `granularity` to the new depth (§6 hazard 2) | `:2598`, `:1449-1465` |

Everything else — F1's inset, the `layer_slices_trimmed` containment guard, the
`opening_ex(last, small_region_threshold)` printability filter, F2's ladder, F3's band, F4's
notch cap — is untouched.

**Resulting normal thickness** `t = D·max(cos θ, sin θ)`, `D = 1.435675` (walls = 3, 0.45 mm
lines, 0.1 mm layers):

| θ | 6.5° | **10°** | **15°** | **20°** | **25°** | 30° | 45° | 60° | 90° |
|---|---|---|---|---|---|---|---|---|---|
| **Option N (mm)** | 1.427 | **1.414** | **1.387** | **1.349** | **1.301** | 1.243 | 1.015 | 1.243 | 1.436 |
| today (mm) | 0.163 | 0.249 | 0.372 | 0.491 | 0.607 | 0.718 | 1.015 | 1.243 | 1.436 |
| gain | 8.8× | **5.7×** | **3.7×** | **2.7×** | **2.1×** | 1.7× | 1.0× | 1.0× | 1.0× |

Never below `D/√2 = 1.015 mm` at any slope. The dead band is gone, the 6.5° cliff is gone,
and the curve is flat to within 30 % from 0° to 90° — the closest practical approach to
"one constant thickness, everywhere" without a variable-radius offset.

**Self-limiting at steep slopes, layer-height-independently.** **[D]** The full-width term at
descent step `m` is non-empty only when `(m+1)·r > wall_stack`, so the whole extension is
inert when `M·r ≤ wall_stack`, i.e. `M·h/tan θ ≤ w+s`. Since `M·h = D` by construction:

> **the descent extension is active only for θ < atan(D / (w+s)) = atan(1.4357/0.8785) = 58.5°**,
> *independent of layer height*. Above that, behaviour is byte-identical to today.

**Gating.** Apply N1–N3 only when `paint_depth_mode != pdmUnlimited` (available as
`print_object.config().paint_depth_mode` inside `segmentation_top_and_bottom_layers`, which
already takes the `PrintObject`), so unlimited mode keeps legacy parity, and only when
`D ≥ wall_stack` — see §5 for why.

---

### Option E — hybrid (candidate (e))

Option N plus Option C's ladder applied only in the 30°–60° window, to lift the 45° dip from
`0.707 D` to `D`. Adds F2 interaction and a second mechanism for a ≤ 30 % improvement in a
band where the user's features do not live (their eyes/cheeks are the shallow end). **Defer.**

---

## 4. Config semantics under Option N (coordinator point 2)

**`paint_depth_mm` becomes the primary control and is redefined as normal thickness.** Key
unchanged, tooltip and header rewritten: "how thick the painted colour is, measured
perpendicular to the painted surface — the same everywhere, on flat tops, curves and vertical
walls alike." Default 1.5 mm (unchanged value, new meaning). This is the number the user
should reach for; their ~2 mm target is a direct entry.

**Walls mode is a convenience conversion to that millimetre value, and it must not assume
Arachne.** The user is on the **classic** wall generator. **[V]** `wall-count-investigation.md`
§2d: on classic, `process_classic` (`PerimeterGenerator.cpp:1341-1359`, `:1376`, `:1466`)
yields **2 external-width loops plus one gap-fill line** for any band in the 1.3–1.5 mm range,
on every layer, regardless of `paint_depth_walls`. F3's band
(`N·s + 2(w_ext−s_ext) + 0.25·s`) is shaped for Arachne's `getOptimalBeadCount` windows and
carries no meaning under classic.

Two defensible readings; **recommend keeping F3's number and re-labelling it**:

* **Keep `D = paint_depth_band_mm(...)` exactly as F3 computes it** (1.435675 at N = 3) and
  document it as "≈ N wall widths of material thickness, measured normal to the surface".
  Rationale: it preserves F3's Arachne count-margin win for the *vertical-wall* case (where
  the painted region genuinely is a `D`-wide annulus and bead counting still decides loops),
  it is only ~10 % above the generator-neutral geometric reading
  `w_ext + (N−1)·s = 1.30708`, and it avoids a second band formula.
* Generator-neutral alternative, if a single honest geometric number is preferred:
  `D = w_ext + (N−1)·s` — literally "N loops of material". Costs Arachne users F3's margin.

**Additional tooltip duties:** state that on classic the *loop count* inside the painted band
is generator-determined and `paint_depth_mm` is the reliable control; and that beyond
`2 × wall_loops` Arachne cannot add loops (`WallToolPaths.cpp:514`) — already documented.

**Deliberate semantic change to surface for approval:** on a **flat top**, depth today is
governed by `top_shell_layers`/`top_shell_thickness` (0.6 mm, 6 layers). Under N2 it becomes
`max(shell, D)` = 1.44 mm ⇒ **15 layers** at 0.1 mm. That is the honest consequence of
"constant thickness `D`", it is what a colour-split object would produce, and
`paint_infill_override` (already present) governs the part of the claim that lands beyond the
solid shell in sparse infill. Keeping `max(shell, …)` rather than plain `D` preserves the
earlier shell-coverage fix wave's contract (never leave base-coloured *solid shell* under
painted skin) when a user sets `D < shell`.

---

## 5. Interaction with F1's no-exterior-bleed invariant (coordinator point 3)

**Option N does not need an exception to F1 — it is F1 that makes it safe.**

**[D] The claim never owns the exterior perimeter of an unpainted sub-surface layer.**
F1's term is kept verbatim: on every inferred (non-surface) layer the full-width claim is
intersected with `offset_ex(input_expolygons[last_idx], −wall_stack)`. So the outermost
`w+s = 0.8785 mm` of every descent layer is claimable **only** by the *lateral* path — and the
lateral path claims a contour arc only where the 2D Voronoi says that arc is painted, i.e.
exactly where the user painted the surface. Below the painted extent, the contour is
unpainted, the lateral claim is absent, and the vertical claim starts ≥ one wall stack in.
Exterior stays base-coloured. **Invariant preserved, unchanged, no argument required.**

**[D] Same conclusion from the sweep geometry (§2), independently.** A point at lateral inset
`u` on layer `k` is generated by painted surface material at height `z_k + u·sin θ·cos θ`. As
`u → 0` (the contour itself) the generating point is at `z_k` — so if the surface at `z_k` is
unpainted, the contour is not claimed at all. The normal-shell rule *by itself* keeps paint
off unpainted exterior; F1 additionally supplies the printability floor at the paint
boundary, where the sweep would otherwise leave a vanishingly thin base strip.

**F1's gate replaces `exposed_surface_part`, and does its job better.** The old gate rejected
a whole patch when it came within one wall stack of the layer above — a *proxy* whose only
real content was "reject steep surfaces", and whose early return (`:1394`) is what let the
bleed through in the first place (taper-bound-review IMPORTANT 1). F1's inset enforces the
actual invariant pointwise, on the descent layer's own contour, and — per §3 Option N — it is
what makes the extension auto-inert above 58.5°. One mechanism, one invariant.

**The one place a gap could open, and how it is closed.** If `D < wall_stack` the lateral band
reaches only `D` while the vertical claim starts at `wall_stack`, leaving a base annulus of
width `wall_stack − D` sandwiched between two painted annuli — a new sliver class. **[D]** At
walls = 1, `D = 0.5786 < 0.8785` ⇒ a 0.30 mm base ring. **Gate the extension on
`D ≥ wall_stack`** (true for walls ≥ 2 and for `paint_depth_mm ≥ 0.88`); below that, keep
today's behaviour — a user asking for sub-wall-stack paint has asked for a paper-thin skin and
should not get a disconnected interior band. (Variant, if walls = 1 must be covered: use
`clearance = min(wall_stack, D)`, contiguous by construction, at the price of a base ring
narrower than a wall stack on layers below the painted extent. Not recommended.)

---

## 6. Implementation hazards (must be in the plan)

1. **The `break` is now in the wrong place** (`:1785`, `:1835`). On a slope the full-width
   term is empty for the *near* descent steps (`(m+1)·r < wall_stack`) and non-empty for the
   far ones. Today's `if (last.empty()) break;` would fire at step 1 and never reach the
   productive steps — the change would be a **no-op** without this. Fix: break only once a
   non-empty full-width term has been seen (the reach `(m+1)·r` is monotone in `m`, so
   "non-empty once ⇒ non-empty thereafter" and termination is still guaranteed by the loop
   bound). Suggested shape: `if (last.empty()) { if (seen_full_width || !extension_enabled) break; else continue; }`.
2. **TBB double-buffer overlap.** `:1449-1465` sizes `max_top_layers` / `max_bottom_layers` /
   `granularity` from *shell* counts, and `granularity` is what keeps two same-parity TBB
   ranges from writing into each other's `shell_triangles_by_color_*[last_idx + layer_idx_offset]`
   slots. A `D`-driven descent is deeper than the shell, so **`granularity` must be widened to
   the new depth** or the parity trick races. This is a correctness bug, not a perf note.
3. **The existing anti-smear test's fixture.** `make_square_frustum(40, 22, 6)` at 0.3 mm
   layers = 33.7°. Under **Option N** it uses `pdmMillimeters 0.15` ⇒ `M = ceil(0.15/0.3) = 1`
   ⇒ no descent at all ⇒ **stays green, untouched**. Under **Option D** the shell bound gives
   `S·r = 1.80 mm > wall_stack` and its 1.0 mm negative probe flips ⇒ the test must be
   re-scoped. This asymmetry is a real argument for N over D.
4. **`opening_ex(last, small_region_threshold)`** at `:1784`/`:1834` stays — it is the
   per-step printability filter on the *combined* claim and is the last line of defence
   against a sub-0.225 mm painted strip surviving to the perimeter generator.
5. **Variable layer height**: use the `print_z`/`bottom_z` walk (`:1290-1338` pattern), not
   `D/h`, so a variable-height object gets the right depth.
6. `pdmUnlimited` must stay byte-identical (`verify_paintdepth.sh` C-checks + the
   "unlimited mode leaves the same painted face unbounded" test) — hence the mode gate.

---

## 7. Costs

**Material: none wasted; it is exactly the shell volume.** The claim is the set of material
within `D` of the painted surface — the minimum volume that can deliver `D` of opacity.
Filament shifts from base to painted colour; total extrusion is unchanged.

**Lateral footprint grows a lot on shallow slopes** — `D/tan θ` = 8.14 mm at 10°, 5.36 mm at
15°, 3.94 mm at 20°. That is correct (normal thickness is still 1.4 mm) but it is visually
striking in preview and should be expected, not treated as a bug.

**Tool changes / purge — the real print-time cost.** **[D]** On a painted flat top the number
of layers carrying a painted region goes 6 → 15 at 0.1 mm layers (4 → 8 at 0.2 mm). Each such
layer costs one extra tool change and its purge. On a slope the layer count is unchanged (the
paint was already present on every sloped layer via the lateral band); only the region gets
wider. So the cost is concentrated on **flat/near-flat painted faces**, and it is a direct
consequence of the constant-`D` semantics the user asked for. If it proves unacceptable, the
dial is `paint_depth_mm` (or fall back to Option D, which has zero flat-top cost).

**Slice time.** The descent no longer breaks at step 1 on slopes: up to `M−1 = 14` Clipper
offset+intersect pairs per painted surface layer instead of 1. Each operates on
`top_ex ∩ layer` (small), the stage is TBB-parallel, and `granularity` grows with it. Expect
the top/bottom segmentation stage to dominate paint-depth slice cost on organic models;
measure with `spike/verify_paintdepth.sh` before/after rather than estimating.

**Sliver / dimple (#7104, #7235) risk: low, and lower than today's alternatives.** Three
independent guards remain: F1's one-wall-stack inset (base at the contour is nothing or
≥ `w+s`), the `D ≥ wall_stack` gate (no sandwiched base ring), and the per-step
`opening_ex(last, small_region_threshold)`. The steep-slope regime that motivated the
anti-smear guard is auto-suppressed above 58.5° by the F1 inset itself. The one class **not**
covered — unchanged from today — is F2's noted Voronoi wrap-around onto an opposite face at a
thin fin tip; the Option C footnote's painted-seed dilation is the eventual answer.

---

## 8. "Printed how a separate object would print" — what already holds (coordinator point 4)

**Already true today [V]:**

* **Own perimeters.** The painted claim is a separate `PrintRegion` whose `wall_filament` is
  the painted extruder (`PrintApply.cpp:1088-1090`); `Layer::is_perimeter_compatible`
  (`Layer.cpp:184`) compares `wall_filament` first, so it is **never** merged with its parent,
  and `Layer::make_perimeters` takes the single-region branch (`Layer.cpp:257-260`) and
  generates loops over the painted region alone. It already gets its own external perimeter.
* **Own top/bottom solid shells at every colour interface.**
  `PrintObject::has_bounded_paint_depth()` (`Print.hpp:495`) forces `interface_shells` at
  `PrintObject.cpp:1333` (`detect_surfaces_type`) and `:1766` (per-region vertical shells),
  and is threaded into the perimeter generator at `PerimeterGenerator.cpp:622` and `:2273`
  (via `LayerRegion.cpp:224-227`). So a colour boundary in Z is treated as a real
  top/bottom surface and gets solid skin — the "separate object" behaviour, already on
  whenever paint depth is bounded.
* **No tolerance gap.** The two regions share an exact boundary polygon; there is no clearance
  offset anywhere in this path. This is the part of "like separate objects" the user
  explicitly does *not* want, and we already do not have it.

**Not true today — the remaining work:**

* **The claim's *shape* is not a normal-offset shell.** It is `max(lateral band, layer-count
  shell)` — this document's subject. **Option N closes this.**
* **Loop count inside the painted region is generator- and width-dependent.** Classic gives
  2 loops + gap fill for any 1.3–1.5 mm band (`wall-count-investigation.md` §2d); Arachne is
  capped at `2 × wall_loops` (`WallToolPaths.cpp:514`). A separate object would not have this
  coupling. Not addressed here; documented.
* **`only_one_wall_top`** (default false, `PrintConfig.cpp:1228`) additionally reduces painted
  regions on domed tops to a single loop through the `has_bounded_paint_depth` hook. Worth
  asking the user whether they have it on.
* **F1's accepted trade-off** — on a painted flat top that reaches the silhouette, the
  exterior side-wall ring below the top layer is base-coloured. A true separate object would
  paint it. This is the deliberate anti-bleed choice from `193d2e0675` and stays.

**Recorded for later, NOT designed here (per instruction):** the user's second idea — a
workflow that *splits painted colours into distinct objects/parts kept in their spatial
location, with no clearance added*. Different layer of the stack entirely (model/volume
splitting + per-part filament assignment, not segmentation), and it would sidestep every
issue in this document by construction. Open a separate spec when the user wants it.

---

## 9. Recommendation

**Ship Option N**, gated on `paint_depth_mode != pdmUnlimited && D ≥ wall_stack`.

Defaults: `paint_depth_mode = walls` (unchanged), `paint_depth_walls = 3` (unchanged),
`D = paint_depth_band_mm(...)` (F3's formula, unchanged number, re-documented as a normal
thickness), descent bound `max(effective_shell_layers, layers_within_thickness(D))`.
`paint_depth_mm` becomes the headline control at 1.5 mm, documented as normal thickness — the
number to give the user for their ~2 mm target (`paint_depth_mm = 2.0`, no `wall_loops`
prerequisite, unlike the walls-mode route).

Fallback if the flat-top tool-change cost is rejected: **Option D** (same edits, shell bound
kept) — 0.56–0.61 mm across the gap, zero flat-top cost, but it requires re-scoping the
existing anti-smear test.

---

## 10. The tests that pin it

All three are expressible in the committed harness
(`tests/libslic3r/test_paint_depth_clamp.cpp`): `make_square_frustum()` (`:700`),
`FRUSTUM_SLOPED_WALLS` (`:719`), `layer_edge_probe()` (`:633`),
`extruder2_claim_for_layer()` (`:180`), `slice_painted_box()` (`:127`), and the
`paint_depth_test_config()` / `Print::process()` pattern at `:216-245`.

### T1 — shallow slope reaches normal depth (the headline; genuine RED today)

`make_square_frustum(bottom = 40.392, top = 18.0, height = 3.0)` ⇒ run 11.196 mm over 3 mm ⇒
**θ = 15.0°**; paint `FRUSTUM_SLOPED_WALLS`; `pdmWalls`/3; `layer_height = 0.1`
(30 layers). At a mid layer:

* `CHECK(any_contains(claim, layer_edge_probe(obj, k, 3.0)))` — predicted reach
  `D/tan15° = 5.36 mm`. **Fails today** (reach 1.436 mm). ⇒ real RED.
* `CHECK_FALSE(any_contains(claim, layer_edge_probe(obj, k, 6.0)))` — bounds the claim, so the
  test pins a *depth*, not merely "more paint".
* Arithmetic companion: assert `reach·sin(15°) ≥ 0.6` and `≤ D·1.05`, i.e. the normal
  thickness is `D` to within the layer quantisation — the actual contract.

### T2 — F1's no-exterior-bleed invariant survives on a slope (must be GREEN before and after)

Same frustum, but paint **only the top cap** (`{2,3}`) so the sloped walls are unpainted and
carry no lateral claim. On each of the first 5 layers below the cap:

* `CHECK_FALSE(any_contains(claim, layer_edge_probe(obj, k, 0.3)))` — the exterior perimeter of
  a slope the user did not paint stays base-coloured (F1, `wall_stack = 0.8785`).
* `CHECK(any_contains(claim, layer_edge_probe(obj, k, 1.5)))` — but the shell beneath the
  painted cap *is* claimed, so the test cannot be satisfied by simply claiming nothing.

This is the pin the fix-wave report asked for: it fails loudly if any future widening tries to
own the outer perimeter of a sub-surface layer on a curved face.

### T3 — steep-slope suppression, at the derived threshold

Two halves, both cheap:

* **Geometric:** `make_square_frustum(40, 34, 6)` ⇒ run 3 mm over 6 mm ⇒ **θ = 63.43°**, above
  the derived `atan(D/wall_stack) = 58.5°`. Paint the sloped walls, `pdmWalls`/3,
  `layer_height = 0.1`. At a mid layer: `CHECK(claim ∋ probe(1.0))` and
  `CHECK_FALSE(claim ∋ probe(2.0))` — the claim is the lateral band alone (1.436 mm), the
  descent contributed nothing (`M·r = 0.75 < 0.8785`).
* **Regression:** the committed "a steep painted surface gains no deep full-width claim
  (anti-smear guard)" test (`:1060`) must pass **unmodified** — under Option N its
  `pdmMillimeters 0.15` / 0.3 mm layers give `M = 1`, no descent, so it is a genuine untouched
  regression pin rather than a re-scoped one.

**Plus the standing gates:** `[paintdepth]` (36 / 416 baseline), `[chameleon]` (133 / 605,
must be unchanged), `spike/verify_paintdepth.sh` 17/17 with unpainted byte-parity, and a
before/after slice-time comparison on the spike fixtures to quantify §7's Clipper cost.
