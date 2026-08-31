# Support Filament Match v2.3 — Implementation Plan

Spec (binding): docs/superpowers/specs/2026-08-30-support-match-v23-design.md
Conventions: all prior (TDD real RED, build-slot rule incl. idle-node-reuse exemption +
MSBUILDDISABLENODEREUSE=1, off-mode purity, fixtures untouched, [chameleon]).
Baseline: 60 cases / 265 assertions; verify 30 checks. ALL param/logic changes are
support-pass-local — Part 1 brim behavior stays byte-identical (its own param struct).

## Task 1 — Gates + hysteresis (C1, C2, C3, C6) [BrimFilament, Print.cpp, tests]

1. C6 param overrides at the support pass's vote_params setup (after the fallback
   computation): max_runs=8, min_run_mm=1.6. Comment why support differs from brim.
2. C1: `chameleon_collect_layer_filaments(print)` once-per-pass helper → z-sorted
   vector<pair<double, std::set<unsigned>>> (EPSILON-merged) of wall/solid/sparse
   filaments (0-based) per object-layer z across ALL objects; per support layer,
   z-coincidence lookup → free set. apply_bucket_caps signature gains
   (const std::set<unsigned>& free_extruders, double min_len_free_mm); per-bucket
   eff_min = free ? 3.0 : 12.0 (call site constants).
3. C2: bucket with extruder ∈ prev_kept passes at 0.5×eff_min. prev_kept update: if
   this layer committed buckets → set to committed; if everything was gated/trimmed
   away but buckets EXISTED pre-gate → retain previous set for ONE more layer (a
   retention counter, decays after 1); truly empty layers (no partitions attempted)
   leave it untouched (existing behavior).
4. C3: BrimVoteParams.prev_kept (std::set<unsigned>, default empty ⇒ all new branches
   no-ops — Part 1 identical). brim_vote tie path: after tie detected, if exactly one
   of {winner, runner} ∈ prev_kept return it; else fall through to object_area →
   min-id. Same preference in vote_collection_as_unit's majority tie. Wire prev_kept
   into the shared vote_params + both lateral param copies each layer (BEFORE resolver
   lambdas copy them — verify copy timing!).
5. Tests: gate tiers; half-gate for prev_kept; retention across one all-gated layer
   (unit-level via apply_bucket_caps + a wrapper for the update rule if extracted —
   extract the prev_kept update decision as a small pure function for testability);
   tie-prefers-prev_kept (must fail pre-change: ids arranged so min-id gives the wrong
   answer); ring seam pre-merge NOT here (Task 2).

## Task 2 — Vote/geometry fixes (C4, C7, C8) [BrimFilament, Print.cpp, tests]

1. C4: PASS 2 of chameleon_pick_projection_region scans HIGHEST band layer first
   (PASS 1 raw scan stays lowest-first — document the asymmetry: raw = nearest surface
   above; ring = prefer the overhang layer over the wall-gap layer). Cross-layer test:
   rim point ring-hit on layer0(wall) AND layer1(overhang) → layer1 region; must FAIL
   pre-change.
2. C7: in split_polyline_core (loop inputs only): after run build, if ≥2 runs and
   first.extruder == last.extruder, merge last INTO first (prepend points, preserving
   the shared-boundary invariant) BEFORE absorb/guard. Test: 4-sector ring whose seam
   straddles one color → runs ≤ expected, seam sector counted once; boundary-vertex
   invariant holds.
3. C8: replace raw solid_infill_filament/sparse/wall reads in
   chameleon_projection_region_extruder + chameleon_region_extruder with
   PrintRegion::extruder(frSolidInfill / frExternalPerimeter | frPerimeter) semantics
   (read PrintRegion.cpp:10-29 and mirror or call it if a PrintRegion* is reachable;
   note the -1 0-based conversion). Delete unreachable 0-value fallback branches
   (config min=1). Tests where the pure core allows; else documented hand-walk.

## Task 3 — Tree selective descent (C5) [BrimFilament, tests]

1. vote_collection_as_unit returns the full per-leaf-sample histogram + longest
   contiguous minority run length (sampled at sample_mm along each leaf in order).
2. partition_support_entities collection branch: (a) uniform or minority-run <
   threshold (min_run_mm, halved when this column descended last layer — threshold
   passed in via params; column key = collection's bbox center quantized? NO —
   per-support-layer object scope: thread a per-object map<quantized-XY, bool>
   descended_last_layer through the pass like prev_kept; keep simple + documented) →
   current behavior (whole-move or stay); (b) mixed with minority-run ≥ threshold →
   DESCEND: partition each leaf entity individually IN PLACE within the collection:
   matched runs become split paths in out[extruder] (per-leaf flow attrs via
   first_path_of(leaf)), fallback runs SPLICED back at the leaf's position inside the
   collection (no_sort order preserved), leaf originals deleted when replaced; the
   collection pointer stays in support_fills unless emptied (then delete).
3. can_reverse() propagation: rebuilt ExtrusionPaths inherit the source leaf's
   can_reverse (grep the field/ctor; branch walls are appended reversal-disabled —
   verify how a path expresses that and mirror it), else chain_and_reorder makes seam
   blobs.
4. Tests: mixed ring descends and splits per leaf; uniform ring whole-moves
   (pointer-stable); sub-threshold minority stays whole; splice preserves order index;
   can_reverse carried; emptied-shell deletion (no double-free — assert via ownership
   walk in report if not unit-provable).

## Task 4 — Integration + verify + handoff

1. Full build; [chameleon] green; verify_chameleon.sh 3 consecutive runs (30/30).
2. C9 validation aids: ensure the pass summary log line includes gate/trim counters +
   free-set size; add a tiny `spike/inspect_partitions.sh` grep helper (per-layer
   partition emission blocks vs T-commands) for GUI-slice triage.
3. Ledger/memory notes + GUI handoff instructions (both modes; expected: sector-
   faithful nearest_wall, stable nearest_surface columns; supports-on-model still
   fallback — documented v2.4 item).
