# Task 4 report — Manifold partition, island absorption, one-shot split, spike measurements

Branch `feat/color-split`, worktree `C:\Dev\SnapmakerOrcaNext`. Base `9562e9abfd`, commit **`7d4f0d8f66`**
`feat(color-split): Manifold partition with island absorption; spike measurements`.

**Status: DONE_WITH_CONCERNS.** Everything the brief asks for is built, tested and committed; the spike
verdict is *engine A as designed*. Three items need the controller's ruling — the S1 boss expectation, the
second half of the folded depth-clamp follow-up, and `ColorSplit.cpp` passing the plan's size intent. Details
under *Deviations* and *Concerns*.

Spike deliverable: **`.superpowers/sdd/2026-09-01-color-split/spike-report.md`** (not committed — it lives
under `.superpowers`).

## What I implemented

`src/libslic3r/ColorSplit.hpp`

- `struct ColorSplitResult { body; pieces; warnings; depths; }` exactly as specified.
- `partition_by_shells(mesh, shells, absorb_islands, progress)` and
  `split_volume_by_paint(mesh, paint, depths, params, progress)` declarations.

`src/libslic3r/ColorSplit.cpp` (`#include <manifold/manifold.h>` as `MeshBoolean.cpp:12` does)

- A second anonymous-namespace section holding the Manifold plumbing, immediately before the partition:
  `to_manifold64` (MeshGL64, `numProp = 3`, `uint64_t` triVerts, `tolerance = 1e-5`, `Merge()`,
  `AsOriginal()`), `from_manifold`, `require_ok` (maps `Error::Cancelled` → `ColorSplitCancelled`), and
  `faces_by_original_id`.
- `partition_by_shells` — spec §3.8: `rest ← M`, then `(piece, remainder) = rest.Split(shell)` per shell in
  the order `build_color_shells` returns (ascending filament), pieces merged per filament, enclosed islands
  absorbed by most-surface / lowest-filament, `body + Σ pieces` checked against `Volume(M)` within 1e-4,
  progress over the 50…95 band.
- `split_volume_by_paint` — patches → shells → partition, shell warnings merged to the front of
  `result.warnings` (Ruling 10), `depths` carried out with `depth_override_mm` applied.
- **Folded follow-up:** `compute_vertex_depths` clamps `d(v)` to `≥ 0`.
- **Spike instrumentation (brief step 5):** `build_color_shells` emits
  `"Filament k: shell depth reduced (N halvings) on one painted feature to keep the split valid."` when the
  validity fallback *salvaged* a component. Only on salvage, so it never doubles up with the skip note and the
  Task 3 skip test is untouched.

I verified against the real headers/sources before writing: `Split` returns **(intersection, difference)**
(`manifold.cpp:950-967` — the header carries no doc comment, the definition does); `runIndex.size() ==
runOriginalID.size() + 1` (`mesh.h:125-138`); `WithContext` copies the Manifold so `OriginalID()` survives
(`manifold.cpp:169-173`); `Manifold(const MeshGL64&)` exists (`manifold.h:86`); `Decompose()` returns
`{*this}` for a single component and an empty vector for an empty manifold (`constructors.cpp:455`).

## Two defects the spike exposed

1. **Manifold emits zero-length runs.** A `Split` output carries a `runOriginalID` entry for an input that
   contributed *no* faces — measured directly: the painted sphere's leftover core reports
   `run 0 id 4 (shell) tris 1224`, `run 1 id 2 (source) tris 0`. The brief's `faces_by_id[id] += n` inserted a
   zero entry for the source id, so `touches_original` was always true and **no island would ever have been
   absorbed**. `faces_by_original_id` now skips empty runs. Found by instrumenting the failing test, not by
   guessing.
2. **`d(v) ≥ 0` is a guard, not a live branch.** The thickness probe starts 1 µm inside the surface and
   discards any hit closer than 5 µm, so every accepted `t` exceeds 6 µm and `t/2 − 0.002 > 0.001`. The clamp
   is still correct and free (a parallel negative offset keeps each bottom triangle's orientation, so the fold
   guard genuinely cannot catch it) — it just cannot be reached through this probe. Evidence below.

## TDD evidence

**RED** — `cmd /c build_next_wt_tests.bat` with the tests written and no implementation:

```
test_color_split.cpp(608,5):  error C2065: 'ColorSplitResult': undeclared identifier
test_color_split.cpp(608,27): error C3861: 'split_volume_by_paint': identifier not found
test_color_split.cpp(624,5):  error C2065: 'ColorSplitResult': undeclared identifier
test_color_split.cpp(624,27): error C3861: 'split_volume_by_paint': identifier not found
test_color_split.cpp(627,5):  error C1003: error count exceeds 100; stopping compilation
```

Expected: the tests call `split_volume_by_paint` / `partition_by_shells` and name `ColorSplitResult`, none of
which existed yet.

**RED (behavioural, after the implementation compiled)** — `libslic3r_tests.exe "[colorsplit]"`:

```
test_color_split.cpp(548): FAILED: REQUIRE( r.body.indices.empty() )     with expansion: false
test_color_split.cpp(575): FAILED: REQUIRE( shells.size() == 0u )        with expansion: 1 == 0
test cases:  22 |  20 passed | 2 failed
```

The first is the zero-run defect (instrumented, diagnosed, fixed). The second is the folded follow-up's
fixture prediction; diagnosed with a temporary `WARN` that printed
`DIAG thin plate depths: min 1.5 max 1.5 n 8` — see *Deviations*.

**GREEN** — final run against the committed tree (`build/tests/libslic3r/Release/libslic3r_tests.exe`):

```
> libslic3r_tests.exe "[colorsplit]"
All tests passed (219 assertions in 23 test cases)

> libslic3r_tests.exe "[colorsplit_spike]"
  sphere triangles: 99224
  S3 one colour: 0.892969 s, pieces 1, warnings 0
  S3 three colours: 3.82067 s, pieces 3, warnings 0
  S1 boss shell: filament 2, volume 8.28923
  S1 boss: piece volume 8.28922, body volume 16001.1 (whole boss 12.5664, boss above the block 9.42478)
  S3 breakdown (99224 tri, shell 66992 tri): patches 0.0207555 s, shells 0.195021 s
    (of which check_shell/CGAL 0.1004 s = 51.4819% of that stage, 10.9871% of the whole split),
    partition 0.698191 s; shell warnings 0
All tests passed (13 assertions in 2 test cases)

> libslic3r_tests.exe "[paintdepth]"
All tests passed (1568 assertions in 94 test cases)
```

## Spike outcome (full report: `spike-report.md`)

**Engine A as designed.** 0.89 s for one colour and 3.82 s for three on a 99 224-triangle sphere,
single-threaded. Manifold's `Split` is 76 % of the split; the CGAL self-intersection check is **11 %**, so
there is no case for a reduced check set, and nothing pointed at engine B. Partition exactness is confirmed by
the boss (piece == shell to six figures), the pairwise-disjoint three-colour cells and the 1e-4 volume check
that passed on every fixture. `PI/90` gives only 32 040 triangles, so the spike uses **`PI/158` → 99 224**.

Paths taken: no Manifold status error anywhere; the skip-with-warning path fires once (0.15 mm ball); the
self-intersection fallback fires **never** — the fold guard and the fallback share the floor
`min(layer_height, d0)`, so wherever the guard can act it gets there first, and where it cannot the fallback
cannot either.

## Deviations from the brief (rulings wanted)

1. **S1 boss assertion replaced — investigated first, as instructed.** `REQUIRE_THAT(piece, WithinRel(π·4,
   0.05))` cannot pass: the piece is **8.289 mm³** against 12.566 expected. The partition is not at fault
   (piece == shell to six figures); the *shell* is a cup. Three independent reasons, all measured: 1 mm of the
   cylinder is buried in the block and bounded by no painted facet (ceiling is 9.425, not 12.566); the boss
   wall has only two rings after the union, both junction vertices with 45° bisector normals, so no offset is
   ever radial and the fold guard caps the base ring before it crosses the axis (hand calculation of that
   geometry: ≈ 8.4 mm³ vs measured 8.289); and the hollow core opens downwards into the block, so it is not an
   *enclosed* island and §3.8 cannot absorb it. I did **not** loosen the tolerance. The test now asserts
   `piece == shell` within 1e-4, `piece > 0.8 × exposed boss`, and that the piece reaches z = 13 and spans the
   full 2 mm diameter, with the evidence in a comment pointing at the spike report. **Suggested spec fix:**
   §3.8's "a painted 1 mm boss is entirely its colour" holds only for features whose leftover core is fully
   enclosed (free-standing); attached features keep a hidden core in the body colour. Island absorption itself
   is proven by the free-floating sphere test.
2. **Folded follow-up, second half.** "a 40×40×0.003 plate … `build_color_shells` must skip it with a warning"
   does not hold: measured, all 8 vertex depths come back as **D = 1.5**, because the probe discards the
   0.0042 mm exit hit as a self-intersection and `t` stays infinite. The shell is a perfectly valid frustum
   that merely overshoots the plate, so nothing is skipped. The clamp (the follow-up's actual content) is
   implemented, and the test asserts `min(d) ≥ 0`, `max(d) == D`, and that the split still behaves — the
   partition clips the overshoot and the whole plate comes out as filament 2 — with the mechanism in a
   comment.
3. `SPIKE_SPHERE_FA = PI/158` instead of `PI/90` (the brief permits adjusting `fa`; 99 224 triangles).
4. The S1 block runs `extract_color_patches` / `build_color_shells` / `partition_by_shells` separately instead
   of `split_volume_by_paint` — the identical chain, staged so the shell's own volume can be measured next to
   the piece's, which is what §9's S1 asks for.
5. **One extra test**, `colorsplit: the halving floor is what decides skip versus salvage` — the 0.15 mm ball
   is skipped at h = 0.2 and salvaged silently at h = 0.02. Written while chasing coverage for the new
   warning; it pins the guard/fallback floor interaction and explains why the fallback is unreachable here.
6. **One extra spike case**, `colorsplit spike: S3 stage breakdown and the CGAL check share` — required by
   brief step 5 (time `check_shell` separately with `std::chrono`).

## Self-review findings (fixed before committing)

- `from_manifold` had a half-guard (`std::max<size_t>(1, stride)` for the count but a raw `stride` for the
  indexing). Made consistent: no vertices when the stride is zero.
- The `ExecutionContext` reads as if it aborted booleans in flight. It does not — `Split` is an eager op that
  ignores an attached ctx (`manifold.h:147-171`), so cancellation lands *between* shells. Said so in a comment
  rather than leaving a misleading impression; kept the plumbing, which spec rev 2 (M7) calls for.
- `faces_by_original_id` now throws instead of guessing if `runIndex.size() != runOriginalID.size() + 1`, so
  the provenance assumption the island rule rests on is explicit.
- All temporary diagnostics (`std::cerr` in the library, `WARN("DIAG …")` in two tests) removed; the final
  build and the runs above are from the clean tree. Test output is pristine apart from the intended spike
  `WARN`s.

## Concerns

1. **`ColorSplit.cpp` is 610 lines** (was 432; the brief's note says ~430). The plan already earmarks Task 5
   as the split point, and the brief told me to report rather than restructure on my own — so I have not. The
   Manifold plumbing and the partition sit in their own anonymous-namespace section as instructed.
2. **The `"shell depth reduced"` warning's emit line has no fixture coverage.** That *is* the spike's answer —
   no fixture reaches the fallback's salvage branch, for the structural reason above — but it means a
   user-visible string ships untested. It stays as the observability hook for cross-facet self-intersections
   (which the fixture set does not contain). Removing it is a one-line revert if you would rather not carry
   it.
3. The S3 stage breakdown was measured for the one-colour case only; the three-colour figure is end-to-end.
4. Cancellation granularity is one `Split` (~1 s on the 100 k sphere). Fine for a job, worth knowing for the
   Task 8 UI.

---

# Fix round 1 (review verdict: one Important, several minors)

Commit **`f44db8be2d`** `fix(color-split): warn when a filament loses all its area; one export per part`.
All findings addressed; nothing else touched (`partition_by_shells` leaving `result.depths` default stays
ledgered as a documented minor).

## Important — a filament with no piece got no warning either

`piece_parts` only gained a key from a non-empty `Split` piece or an absorbed `Decompose` component, so a
filament whose shells were entirely consumed by lower filaments produced no map entry, no piece and **no
warning** — the `its.indices.empty()` branch that was meant to catch it could never run.

`partition_by_shells` now tallies `std::set<int> shell_states` from `shells` before the Split loop and, after
`r.pieces` is assembled, pushes
`"Filament <k>: painted area produced no solid (fully covered by lower filaments)."` for every shell state
that has no entry in `piece_parts`. The unreachable branch is gone.

Covered by the new `colorsplit: a filament left with no solid of its own is reported`: the cube top is
shelled once, the shell is duplicated with `state = 3`, and `partition_by_shells` returns one piece (filament
2) plus exactly that warning naming filament 3.

## Minors

- **`std::abs(vol_original)`** in the volume-check tolerance (`ColorSplit.cpp`) — a negative source volume
  would otherwise turn the tolerance negative and make the check always throw.
- **One MeshGL64 export per part.** `from_meshgl64(const MeshGL64&)` is now the conversion; `from_manifold`
  is a one-line wrapper over it. The island pass exports each body component **once** (`const MeshGL64 gl =
  comp.GetMeshGL64();`) and feeds that same export to both the provenance tally and the mesh it hands back,
  absorbed islands included. `piece_parts` / `body_parts` hold `(volume, indexed_triangle_set)` pairs built at
  the point where the export already exists, so the assembly loop no longer re-exports anything.
- **Tolerance comment corrected.** `mesh.h:159-162`: the tolerance actually used is the **maximum** of the
  explicit value and a bounding-box baseline, and any edge below it may be collapsed — so 1e-5 cannot switch
  simplification off, it only pins the floor at the spec's value. The old comment claimed the opposite.
- **`effective_depths(depths, params)`** helper in the anonymous namespace; `build_color_shells` and
  `split_volume_by_paint` both call it instead of spelling the `depth_override_mm` rule out separately.
- **Cancellable ingest.** `to_manifold64` takes the `ExecutionContext` and uses `ctx.FromMeshGL(m)`
  (`common.h:262-271`, which has a `MeshGL64` overload) instead of `Manifold(m)`, so the heavy ingest phases
  observe cancellation. `AsOriginal()` still stamps the provenance ID and drops the attachment, so callers
  re-attach with `WithContext`. Used for the source mesh and for every shell.
- **Island attribution test** — `colorsplit: the island where two colours meet is absorbed, not left in the
  body`: a radius-2 sphere painted state 2 on z > 0 and state 3 on z < 0 at unlimited depth. Both shells stop
  δ short of the mid-surface, the enclosed island in the centre is absorbed, `volume_of(r.body) < 1e-6`, two
  pieces (2 and 3), and Σ volumes matches the sphere within 1e-4.
- **Cancellation in the partition band** — the existing case gained a second callback that refuses only above
  50 %, so it cancels inside the Split loop (which ticks 50…90) rather than at `progress(0)`.
- **`test_color_split.cpp`** — the tautological `REQUIRE(duration >= 0.)` in the S3 breakdown case is gone.

## Verification

Command (after `build_next_wt_tests.bat`, exit 0), from
`build/tests/libslic3r/Release/libslic3r_tests.exe`:

```
> libslic3r_tests.exe "[colorsplit]"
Testing colorsplit: a filament left with no solid of its own is reported
Passed in 0.000839 [seconds]
Testing colorsplit: the island where two colours meet is absorbed, not left in the body
Passed in 0.035384 [seconds]
All tests passed (230 assertions in 25 test cases)

> libslic3r_tests.exe "[colorsplit_spike]"
  sphere triangles: 99224
  S3 one colour: 0.928305 s, pieces 1, warnings 0
  S3 three colours: 4.00089 s, pieces 3, warnings 0
  S1 boss shell: filament 2, volume 8.28923
  S1 boss: piece volume 8.28922, body volume 16001.1
  S3 breakdown (99224 tri, shell 66992 tri): patches 0.0209573 s, shells 0.201463 s
    (of which check_shell/CGAL 0.104644 s = 51.942% of that stage, 11.0127% of the whole split),
    partition 0.727796 s; shell warnings 0
All tests passed (12 assertions in 2 test cases)

> libslic3r_tests.exe "[paintdepth]"
All tests passed (1568 assertions in 94 test cases)
```

`[colorsplit]` went from 23 cases / 219 assertions to **25 / 230**; `[colorsplit_spike]` from 13 to 12
assertions (the tautological one removed); `[paintdepth]` unchanged at 94 / 1568.

The spike numbers are unchanged by the refactor — S1 is 8.28923 / 8.28922 to the digit and the CGAL share is
11.0 % as before — so `spike-report.md` still stands as written; the S3 wall times moved only by run-to-run
variance (0.93 / 4.00 s against 0.89 / 3.82 s).

## Status of the items flagged for a ruling

- **S1 boss — RULED, no longer open.** Spec rev 2.4 (`d1cc74fee9`) takes the finding: §3.1a adds a refinement
  pre-pass (Ruling 13) that refines F so no edge exceeds `L = max(ws, min(D_eff, bbox_diagonal/20))`,
  citing spike S1 by name — STL cylinders and bosses have only two vertex rings, so without interior vertices
  no offset is ever radial and a painted boss becomes a cup. Ruling 14 adds the concave-crease rule. Both land
  in a later task; the S1 case in this file will need its numbers re-recorded once §3.1a is built (the spike
  report says the same).
- **Folded depth-clamp follow-up, second half — still open.** A 3 µm plate measures `t = ∞` (the probe
  discards its 4.2 µm exit hit), so `d = D` and nothing is skipped. The clamp itself is in; the test pins the
  measured behaviour.
- **File size — still open.** `ColorSplit.cpp` is now **640 lines**, further past the plan's ~430 intent.
  Task 5 remains the earmarked split point; not restructuring on my own.
