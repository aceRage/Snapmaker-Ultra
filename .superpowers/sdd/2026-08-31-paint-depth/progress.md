# SDD ledger — plan: docs/superpowers/plans/2026-08-31-paint-depth.md

Spec: docs/superpowers/specs/2026-08-31-paint-depth-design.md (binding; user decisions §User decisions)
Worktree: C:\Dev\SnapmakerOrcaNext · branch feat/paint-depth · BASE = plan commit

## Pre-flight notes
- T1 owns worktree build setup (no build/ yet; mirror supports-worktree config vs main deps).
- T1/T2 seam: T1's paint_depth_band_mm helper is T2's input; T2 consumes config via the new options directly (old key legacy-only).
- Bounded-by-default = OUTPUT CHANGES for painted models by design (user decision); UNPAINTED models must stay byte-identical (T4's parity check is the hard gate).
- [paintdepth] new test tag; chameleon suite must stay green (shared files possible in PrintConfig/Tab).
- Known upstream caveat #6892 (thin parts) tracked to T2/T4.

## Progress
- T1 DONE ed726d71e9: worktree build configured (VS17/x64 vs main deps, BUILD_TESTS=ON, wrapper bats in scratchpad); paint_depth_mode [unlimited|walls|millimeters] default walls / paint_depth_walls 3 / paint_depth_mm 1.5; interlocking default 0.3; handle_legacy nonzero mmu_segmented_region_max_width → millimeters mode (old key legacy-parse-only, REMOVED from Tab UI); paint_depth_band_mm helper RED→GREEN. [paintdepth] 4/10; [chameleon] 133/605 unchanged; ALL_BUILD exit 0.
- T2 DONE: multi_material_segmentation_by_painting now derives its clamp band via paint_depth_band_mm from every printing region's flow (max across regions); interlocking_depth explicitly zeroed for pdmUnlimited (Task 1's 0.3 default would otherwise leak a clamp into "legacy" mode); whole-layer short-circuit (:2149-2151) needed no code change — already flows through the same cut_segmented_layers gate, confirmed by a real end-to-end test that reproduces has_layer_only_one_color directly. New tests/libslic3r/test_paint_depth_clamp.cpp: 4 E2E [paintdepth] cases (real painted mesh -> sliced PrintObject -> inspected applied region geometry), real TDD RED confirmed via git-stash bisection. #6892 thin-feature fixture not attempted (documented GUI-round note instead). [paintdepth] 8/40 stable; [chameleon] 133/605 unchanged; ALL_BUILD exit 0. Report: task-2-report.md.
