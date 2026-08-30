# Support Filament Match v2.1 — Implementation Plan

Spec (binding): docs/superpowers/specs/2026-08-30-support-match-v2-design.md
Base: the branch tip after the "Default" label commit. All v2.0 conventions carry over:
0-based engine keys, 1-based ToolOrdering pushes, run-stable ordinals, object coords
(no instance shift) in the pass, off-mode byte-purity, visited-flag idempotency,
[chameleon] tag, TDD with real RED evidence, build-slot protocol.

## Task 1 — Engine generalization (BrimFilament.{hpp,cpp} + tests)

1. `BrimVoteParams.max_dist_mm` (double, default 0 = uncapped). In `brim_vote`: if the
   nearest returned knn sample's distance exceeds the cap, return fallback_extruder
   BEFORE scoring. Part 1 brim path passes 0 — behavior identical (prove with existing
   suite unchanged).
2. Resolver-driven splitting: add
   `std::vector<BrimRun> split_polyline_by_resolver(const Points&, bool is_loop, const std::function<unsigned(const Point&)>& resolver, const BrimVoteParams&)`
   — same sampling (sample_mm), absorb (min_run_mm), max_runs, shared-boundary-vertex
   semantics as `split_polyline_by_vote`; refactor the two to share the run-building
   core (vote variant = resolver variant with a knn-vote lambda; keep the public vote
   API intact for Part 1).
3. Generalize the partition function:
   `size_t partition_support_entities(ExtrusionEntityCollection& support_fills, ExtrusionRole role_filter, unsigned fallback_extruder, const std::function<unsigned(const Point&)>& resolver, const BrimVoteParams& p, std::map<unsigned, ExtrusionEntityCollection>& out)`
   — same fast-path/ownership/deferred-reinsert semantics as
   `partition_support_interfaces`; emitted split paths copy `first_path_of(entity)->role()`
   (fall back to the entity's `role()`); keep `partition_support_interfaces` as a thin
   wrapper (role_filter = erSupportMaterialInterface, knn resolver) so T2/T3 unit tests
   stay valid until Task 3 rewires, then it may be removed if unused.
4. `select_contact_layers` gains a sibling
   `select_layers_in_band(print_zs, lo_z, hi_z)` (top-z ∈ (lo, hi]); reimplement
   `select_contact_layers` on it.
5. Unit tests (TDD, [chameleon]): max_dist cap returns fallback beyond 1 mm and votes
   within; resolver splitting produces runs matching a synthetic resolver; base-role
   entity partitioned by `partition_support_entities` keeps erSupportMaterial on its
   split paths and never touches interface entities when role_filter = base (and vice
   versa); select_layers_in_band coplanar case.

## Task 2 — Projection resolver (Print.cpp + tests where extractable)

1. New helper `chameleon_projection_extruder(const PrintObject&, const std::vector<size_t>& band_layer_indices, const Point& p, unsigned& out_extruder) -> bool`:
   iterate band layers LOWEST first; bbox-gate then point-in-ExPolygons on
   `layer->lslices`; on hit, pick the LayerRegion whose `slices` contain p — prefer a
   region with an stBottom/stBottomBridge surface in `fill_surfaces` covering p when
   more than one region contains it; resolve filament via the exact Part 1 rules used by
   `chameleon_collect_wall_samples` (outer_wall_filament for external-perimeter-bearing
   regions is NOT the right analogue here — for a surface the region's own
   wall_filament/outer_wall_filament resolution applies to its walls; for the surface
   COLOR use the region's config resolution mirroring chameleon_collect_wall_samples'
   extruder derivation, object default as last resort). Pure-ish function over layer
   data; unit-test the geometric selection with hand-built ExPolygons if Layer can't be
   instantiated — otherwise cover via Task 3 integration + code walk in report.
2. Perf guard: precompute per-band-layer bboxes once per support layer; note complexity
   (samples × band layers) in a comment; bail to lateral on any degenerate data.

## Task 3 — Pass rewiring + emission role (Print.cpp, GCode.cpp)

1. In `chameleon_assign_support_interfaces` (rename comment header to "support match",
   keep function name): per support layer build (a) the contact-band index data for
   PROJECTION (band layer indices; drop the contact-band WallSampleIndex), (b) a
   coplanar WallSampleIndex over `select_layers_in_band(print_zs, print_z - height, print_z)`
   walls for the LATERAL rule with `max_dist_mm = 1.0` in a copy of the vote params.
2. Interface resolver lambda: projection hit → that extruder; else lateral vote (cap) —
   the lateral call itself returns fallback beyond cap. Base resolver: lateral only.
3. Two engine calls per support layer sharing one `out` map and one switch count:
   interfaces first, then base (`role_filter = erSupportMaterial`). Base fallback =
   the object's resolved support_filament (mirror the scalar approximately, as v2.0 did
   for interfaces; document). Plate guard/visited/caps/logging unchanged — one summary
   line gains a `base_runs_matched` counter.
4. GCode.cpp emission: pass erMixed to `extrude_support` for the partition map (verify
   erMixed lets per-path roles drive flow/speed — read extrude_support; if erMixed
   misbehaves, split the map iteration by role and pass the explicit role per subset;
   record which branch was taken and why).
5. Full app build + [chameleon] green + verify_chameleon.sh (off-mode checks must stay
   green untouched; nearest_surface sanity checks re-run).

## Task 4 — Verification + handoff

1. Extend verify script: a nearest_surface tshape slice must still exit 0; add a
   projection-determinism check (two runs, support-scoped diff empty).
2. Re-slice guidance for the user's GUI fixture documented in the task report: the 9
   forensic flip-flop pairs (z 28.92/29.04, 30.96/31.08, 35.52/35.64, 38.76/38.88,
   44.28/44.4, 54.24/54.36, 58.32/58.44, 60.72/60.84, 64.8/64.92) must come out
   same-tool per column; near-wall base matches walls; "Default" label visible.
3. Ledger + memory updates staged for the controller.
