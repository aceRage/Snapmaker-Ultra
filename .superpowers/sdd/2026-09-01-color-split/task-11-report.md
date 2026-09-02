# Task 11 — Verification, docs and ledger (+ Tasks 9-10 review polish)

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/color-split`, base `cadeee2731`.

Commits:

| SHA | Subject |
|---|---|
| `99fb7ab2f7` | `fix(color-split): review polish (Tasks 9-10 minors)` |
| `0ebb9854a1` | `docs(color-split): spec status implemented (v1), measured results` |

Both carry the `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` trailer (verified with
`git log -1 --format=%B`). Nothing under `.superpowers/` was committed; the pre-existing uncommitted
`progress.md` edit and the untracked worktree junk (`one_vertex_touch.svg`, `twospheres.obj`, `spike/out/*`)
were left alone.

---

## 1. Polish batch (commit `99fb7ab2f7`, 10 files, +83 / -29)

### Item 1 — Ruling 27(2): "nothing to split" is a notification, not a modal

- `src/libslic3r/ColorSplit.hpp:19-29` — new `enum class ColorSplitErrorKind { generic, nothing_to_split }`
  and `ColorSplitError` gains an explicit `(msg, kind = generic)` constructor with a public `kind` member.
  `ColorSplitCancelled` is unchanged (it takes the default).
- `src/libslic3r/ColorSplit.cpp:76` (`No filaments to derive the split depth from.`) and
  `src/libslic3r/ColorSplit.cpp:192-193` (`The part has no painted colours.`) pass
  `ColorSplitErrorKind::nothing_to_split`. Every other throw site keeps the default.
- `src/slic3r/GUI/Plater.cpp:24050-24056` — the pre-job catch routes `nothing_to_split` to
  `get_notification_manager()->push_plater_warning_notification(e.what())` and everything else to
  `show_error`.
- `src/slic3r/GUI/Jobs/ColorSplitJob.hpp:47-48` — `Target::error_kind` (default `generic`);
  `src/slic3r/GUI/Jobs/ColorSplitJob.cpp:75-78` — a `ColorSplitError` catch ahead of the `std::exception`
  one records the kind; `ColorSplitJob.cpp:105-112` — `finalize()` pushes the wrapped message into
  `warnings` instead of `errors` for that kind. The message wording
  (`Could not split "%1%" by colour: %2%`) is unchanged; only the channel differs.

No message-text matching anywhere.

### Item 2 — Ruling 27(3): first part's depths, part count, per-target depth validation

- `src/slic3r/GUI/Plater.cpp:24040-24043` — `shown_depths` is now taken from the FIRST painted part
  (`if (targets.empty())`) instead of being overwritten by each part in turn.
- `src/slic3r/GUI/ColorSplitDialog.hpp:27-32` / `.cpp:19-27` — new `size_t part_count` constructor
  parameter (passed as `targets.size()` from `Plater.cpp:24069`); `.cpp:36-38` adds the summary line
  `"%1% painted parts (the depths below are the first part's)"` when `part_count > 1`, via `format_wxstr`
  (`#include "format.hpp"` added).
- `src/slic3r/GUI/Plater.cpp:24075-24084` — after the dialog and before `replace_job`: when
  `! dlg.unlimited()` and no override was given, every target must have `depths.D > 0`, otherwise a
  plater warning `Part %1% has no paint depth configured; tick Unlimited depth or enter an override.`
  and no job. (`t.depths.D` is the world depth times the split space's positive scale, so the test is
  equivalent to testing the world depth.)
- `src/slic3r/GUI/ColorSplitDialog.cpp:116-119` — the dialog's own single-part version of that check was
  removed, so the condition has exactly one home; the now-unused `m_depths` member went with it
  (`ColorSplitDialog.hpp:47`). "Unlimited depth" already applied to all targets
  (`Plater.cpp:24089`, `t.depths.unlimited = dlg.unlimited()`), unchanged.

### Item 3 — validate the override only when it is enabled

`src/slic3r/GUI/ColorSplitDialog.cpp:101-105` — `validate()` returns true immediately when
"Unlimited depth" is ticked, so a stale/invalid value in the greyed-out field can no longer block OK.
`params()` already zeroed the override in that case.

### Item 4 — generic exception net around the config/depth computation

`src/slic3r/GUI/Plater.cpp:24057-24061` — `catch (const std::exception &e) { show_error(...); return; }`
after the `ColorSplitError` handler (derived-first order).

### Item 5 — job's final progress line

`src/slic3r/GUI/Jobs/ColorSplitJob.cpp:88` — `ctl.update_status(100, _u8L("Split by painted colour done."))`.
The per-target `status` string is still used by the progress lambda.

### Item 6 — the mitre floor is named

`src/libslic3r/ColorSplitShell.cpp:473-476` — `constexpr float BISECTOR_MITER_COS_FLOOR = 0.5f;` declared at
its point of use in `ShellBuilder::bisector_length` (the same local-constexpr style as `CREASE_TIE_EPS`,
`CREASE_COS_15` and `CREASE_MITER_LIMIT`; it cannot literally sit beside `CREASE_TIE_EPS`, which is scoped
inside `group_topology`'s loop). The doc comment above now names it and points at `CREASE_MITER_LIMIT`.

### Item 7 — the stale `grid_box_top` rationale

`tests/libslic3r/test_color_split.cpp:1330-1334` — rewritten: the grid box is used because its interior
vertices carry vertical normals and walk straight down, giving a flat-topped slab exactly D deep whatever
the mitre does; a plain cube's top has only corner vertices, whose mitred bisectors do bury D perpendicular
(Ruling 24) but also travel D inward in x and y, making a tapered wedge rather than the full-footprint slab
these bounding-box measurements want. The old "corner bisectors drop only D/sqrt(3)" claim is gone.

### Item 8 — the cube facet table comment — PREMISE DID NOT HOLD, see §4

The brief (repeating the Task 6 reviewer) said `tests/libslic3r/test_paint_depth_clamp.cpp:39-52` "has the
Y faces swapped (6,7 are -Y and 10,11 are +Y per `its_make_cube`)". Decoding `its_make_cube`
(`src/libslic3r/TriangleMesh.cpp:886-896`): facets 6,7 are `{1,7,6}`/`{1,6,2}`, whose vertices are all at
y = 0 (-Y); facets 10,11 are `{4,0,3}`/`{4,3,5}`, all at y = y (+Y). The clamp table already said
`6,7 = Y=0 side` and `10,11 = Y=y side` — i.e. it was **correct**, just written by plane rather than by
sign. Swapping it as instructed would have made it wrong, so instead:

- `tests/libslic3r/test_paint_depth_clamp.cpp:42-43` — relabelled to the sign notation the rest of that file
  and `test_color_split.cpp` use: `6,7 = -Y side (y=0)`, `10,11 = +Y side (y=y)`. Same facts, no ambiguity.
- `tests/libslic3r/test_color_split.cpp:22-23` — this was the actually-false comment, and the likely source
  of the reviewer's note: it claimed "The Y pair is the other way round from the table
  test_paint_depth_clamp.cpp:39-52 carries". The two tables agree; the note now states both facet
  triples and says so.

Comment-only in both files; no test logic touched. `[paintdepth]` is byte-for-byte green (§3).

---

## 2. Build

Slot protocol: `Get-Process cl,link,MSBuild` before the build reported none running (slot free).

```
cmd /c "C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad\build_next_wt.bat"
```
**exit code 0.** Log: `…\scratchpad\build_task11.log` (74 lines). Recompiled `ColorSplit.cpp`,
`ColorSplitPartition.cpp`, `ColorSplitShell.cpp`, `ColorSplitDialog.cpp`, `ColorSplitJob.cpp`, `Plater.cpp`,
`test_color_split.cpp`, `test_paint_depth_clamp.cpp`; relinked `libslic3r.lib`, `libslic3r_gui.lib`,
`Snapmaker_Orca.dll`, `snapmaker-orca.exe`, `libslic3r_tests.exe`, `fff_print_tests.exe`. Zero
`error C…`/`error LNK…`; the only warnings are the pre-existing LNK4098/LNK4286/LTCG ones on targets I did
not touch. No source was edited after this build.

## 3. Test runs (all from `C:\Dev\SnapmakerOrcaNext\build\tests\libslic3r\Release\`), summary lines verbatim

| # | Command | Summary line |
|---|---|---|
| 1 | `libslic3r_tests.exe "[colorsplit]"` | `All tests passed (912 assertions in 56 test cases)` |
| 2 | `libslic3r_tests.exe "[colorsplit]"` (repeat) | `All tests passed (912 assertions in 56 test cases)` |
| 3 | `libslic3r_tests.exe "[colorsplit_spike]"` | `All tests passed (24 assertions in 3 test cases)` |
| 4 | `libslic3r_tests.exe "[paintdepth]"` | `All tests passed (1568 assertions in 94 test cases)` |
| 5 | `libslic3r_tests.exe "[chameleon]"` | `All tests passed (605 assertions in 133 test cases)` |
| 6 | `libslic3r_tests.exe` (full) | `test cases:   581 |   579 passed | 2 failed as expected` / `assertions: 52583 | 52581 passed | 2 failed as expected` (exit 0) |

Runs 1 and 2 are identical — no nondeterminism. The 2 "failed as expected" cases in the full run are the
known xfails that pre-date this feature.

## 4. Measured numbers (recorded in `spike-report.md`, section "Final verification run (Task 11)")

- **S1 boss** (`depths_for_test(1.5)`, default params): side-tube shell 9.37426 mm³, top-slab shell
  4.43006 mm³, piece after the partition **9.37566 mm³ = 99.48 %** of the 9.42478 mm³ of boss above the
  block; body 16000 mm³ (whole boss 12.5664 mm³). Bit-identical to the Ruling 24/25 measurement.
- **S3 timing** (99 224-triangle sphere): one colour **1.01306 s** (1 piece, 0 warnings), three colours
  **6.52579 s** (3 pieces, 0 warnings). Breakdown at 69 520 shell triangles: `extract_color_patches`
  0.0199538 s, `build_color_shells` 0.229673 s (of which CGAL `check_shell` **0.103846 s** = 45.2147 % of
  that stage, **10.3237 % of the whole split**), `partition_by_shells` 0.756268 s. Within noise of Ruling 24.
- **S4a parity** (layers 25-74, D = 1.40885 mm, ws = 0.79708 mm, notch zeroed): 2D odd **54.3691 mm²**,
  2D even **54.3691 mm²**, 3D **55.9797 mm²**; worst difference **1.61063 mm² of the 4.0 mm² bound (40 %)**,
  matching the derived case-B corner hold ws·(2D − ws) = 1.61059.
- **S4b wedge** (layer 98, print_z 19.8): `crease_step` off body **47.6398 mm²** / piece 1552.36 mm²; on
  body **124.991 mm²** / piece 1475.01 mm²; one wall-stack ring 127.533 mm² at ws = 0.79708 mm.

## 5. Docs (commit `0ebb9854a1`)

`docs/superpowers/specs/2026-09-01-color-split-design.md`:
- Line 3 — Rev 2.12 → **2.13**, and `Status: awaiting user review, spike pending` →
  **`Status: implemented (v1) — GUI round pending`**.
- New **`## 12. Measured`** (52 lines): the six test summaries; S1, S3, S4a, S4b as above; the four
  documented limits (per-vertex crease classification; the one-wall-stack hold on four-vertex faces it
  implies, ≈1.61 mm²/layer, which is the whole S4a residual; first-instance depth for anisotropic
  multi-instance objects, §3.9; sub-2·ws strokes falling back to the bisector, §3.6 width guard/Ruling 22),
  plus a pointer to §3.10's exclusions; and a closing "Not yet measured" naming the GUI round that has not
  been walked by hand. Factual, no marketing.

The rev bump follows this spec's own convention (2.10/2.11/2.12 were each bumped by their doc commit).

## 6. Files changed

`src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `src/libslic3r/ColorSplitShell.cpp`,
`src/slic3r/GUI/ColorSplitDialog.hpp`, `src/slic3r/GUI/ColorSplitDialog.cpp`,
`src/slic3r/GUI/Jobs/ColorSplitJob.hpp`, `src/slic3r/GUI/Jobs/ColorSplitJob.cpp`,
`src/slic3r/GUI/Plater.cpp`, `tests/libslic3r/test_color_split.cpp`,
`tests/libslic3r/test_paint_depth_clamp.cpp`, `docs/superpowers/specs/2026-09-01-color-split-design.md`.
Not committed (written only): `.superpowers/sdd/2026-09-01-color-split/spike-report.md` (appended),
`.superpowers/sdd/2026-09-01-color-split/task-11-report.md` (this file).

## 7. Self-review

- All eight polish items are done; item 8 differs from the brief's instruction because its premise was
  false — see §1 item 8 for the derivation. Both affected comments now state the same, verified facts.
- Behaviour changes are confined to the polish: error channel (modal → notification) for the two
  nothing-to-split errors, the new per-target depth refusal, the relaxed override validation, the dialog's
  extra summary line and part-count parameter, and the "done" progress string. Library behaviour is
  unchanged: `ColorSplitShell.cpp` is a rename of a literal, `ColorSplit.cpp` only adds an argument that no
  library code reads, and both test edits are comments — confirmed by `[colorsplit]`, `[colorsplit_spike]`,
  `[paintdepth]` and `[chameleon]` all reproducing their previous counts and every pinned number.
- Test output is pristine: no new WARNs, no skips; the WARN lines present are the deliberate measurement
  reports (S1/S3/S4/parity).
- Derived-first catch order in `split_by_color` (`ColorSplitError` before `std::exception`) is correct, and
  `ColorSplitCancelled` — which derives from `ColorSplitError` — is not thrown on that path.

## 8. Concerns

1. **Item 8's premise was wrong** (detailed in §1). I did not swap the table, because swapping it would
   have introduced the error the reviewer thought was there. If the controller wants the literal swap it
   should be re-checked against `its_make_cube` first.
2. **The dialog's own "no depth" modal is gone**, by design: Ruling 27(3) puts that validation after the
   dialog and asks for a notification naming the part, and the dialog cannot name parts it does not show.
   The cost is that a user who unticks "Unlimited depth" with no depth available now learns of it after
   pressing OK rather than while the dialog is open.
3. **The GUI has still never been run.** Everything in Task 10 and in this polish batch is verified by
   compilation and by the library tests underneath it; the spec's GUI round (paint → split → slice →
   undo/redo → 3MF round trip → gizmo-open refusal) remains outstanding, which is why the status line says
   "GUI round pending".
4. **The `Could not split "%1%" by colour: %2%` wrapper is reused for the warning channel** in the job, so
   a nothing-to-split target reads "Could not split …" inside a warning notification rather than an error
   box. Routing is what Ruling 27(2) asked for; the wording can be softened if the controller prefers.
