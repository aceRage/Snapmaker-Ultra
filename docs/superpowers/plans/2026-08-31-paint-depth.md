# Paint Depth — Implementation Plan

Spec (binding): docs/superpowers/specs/2026-08-31-paint-depth-design.md
Worktree: C:\Dev\SnapmakerOrcaNext · branch feat/paint-depth · BASE c345859f55
Conventions (proven on the supports feature): TDD with real RED, build-slot protocol
(idle MSBuild node-reuse workers exempt; MSBUILDDISABLENODEREUSE=1), off-behavior
purity where a mode disables the feature, [paintdepth] test tag (new), per-task reports
+ scoped reviews, fixtures never modified.

## Task 1 — Config surface + walls math (PrintConfig, UI wiring, pure helper, tests)

1. Options (Print Settings > Multimaterial): `paint_depth_mode` coEnum
   [unlimited|walls|millimeters] default WALLS; `paint_depth_walls` coInt default 3
   min 1; `paint_depth_mm` coFloat default 1.5. Tooltips explain the bleed rationale.
2. Legacy: handle_legacy maps a NONZERO stored mmu_segmented_region_max_width to
   {mode=millimeters, paint_depth_mm=value} (zero → defaults, i.e. walls mode — the
   approved bounded-by-default flip). Decide whether the old key remains the internal
   carrier or is fully replaced; prefer full replacement with the new options feeding
   the segmentation call directly, keeping the old key parseable via handle_legacy only.
3. mmu_segmented_region_interlocking_depth default 0 → 0.3 (active only when depth
   bounded, as today's gating already ensures).
4. Pure helper (BrimFilament is the wrong home — put it beside the segmentation code
   or a small new header): `paint_depth_band_mm(mode, walls, mm, ext_perimeter_width,
   perimeter_spacing)` — walls mode = ext_perimeter_width + (walls-1)*perimeter_spacing
   (fuzzy-skin precedent MultiMaterialSegmentation.cpp:2237-2253); unit tests
   ([paintdepth] tag) for all modes + edge cases (walls=1, zero widths).
5. UI: Tab.cpp Multimaterial group (beside the existing mmu/interlocking rows);
   ConfigManipulation greying (walls/mm fields follow mode); Preset registration;
   invalidation joins posSlice keys (PrintObject.cpp:957-972 group).
6. SETUP (first action of this task): the worktree has no build tree — configure
   cmake against the main checkout's deps exactly like the supports worktree
   (CMAKE_PREFIX_PATH=C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local),
   generator matching C:\Dev\SnapmakerOrca\build's cache, build dir build/; write a
   wrapper bat in the session scratchpad. Full app build exit 0 is this task's gate
   (PrintConfig touched).

## Task 2 — Clamp wiring (MultiMaterialSegmentation.cpp, tests)

1. multi_material_segmentation_by_painting (:2197-2234): compute the band via the
   helper from the object's regions' flows (painted region flow; document which region
   supplies widths when multiple — max width is the conservative choice), feed the
   existing cut_segmented_layers gate (:2169-2172) whenever mode != unlimited.
2. Whole-layer short-circuit (:2149-2151): a fully-ringed layer must still clamp to
   the band (route it through the cut path or band-diff it explicitly).
3. Beam-interlocking mutual exclusion (:2169) preserved; interlocking sub-band applies
   per its existing implementation.
4. Tests: segmentation-level if the entry points are instantiable with synthetic
   meshes/paint (investigate — TriangleSelector state constructible in tests?); else
   pure-geometry tests of the band/cut math + a documented hand-walk; plus config
   plumb-through pins where reachable.
5. Thin-feature caveat (#6892): build a thin-wall painted fixture in tests if
   feasible; else document as GUI-round check.

## Task 3 — Stage 2 vertical bleed (PrintObject/PrintApply, tests)

1. Z-interface solid skin: smallest correct form — when an object has any bounded
   painted region, treat color-boundary interfaces as needing solid skin (evaluate:
   flipping interface_shells semantics for these objects vs a scoped classification
   change at PrintObject.cpp:1373-1376; pick per code reality, document trade).
2. `paint_infill_override` coBool default true; false ⇒ bounded claims keep base-color
   SPARSE infill (conditional at PrintApply.cpp:1076-1078 — sparse only; walls+solid
   stay painted). UI + invalidation + tests (region-config level pins).

## Task 4 — Verification + GUI handoff

1. New spike/verify_paintdepth.sh: slice an UNPAINTED fixture with defaults →
   byte-parity vs a pre-feature baseline (feature must be inert without paint);
   painted-fixture CLI checks only if a loadable painted 3mf exists (GUI-exported
   project 3mfs segfault the CLI — known; try a minimal hand-built 3mf with
   mmu_segmentation attributes, else document GUI-only).
2. Full suite + [paintdepth] green ×2; report with GUI instructions: paint a dark
   spot on a light thick body, cross-section preview shows ≤3 dark walls + base
   interior; toggle modes; interlocking visible at the boundary; Stage 2 solid skin
   above/below the claim.
