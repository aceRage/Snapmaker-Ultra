# Support Filament Match v2.2 — Implementation Plan

Spec (binding): docs/superpowers/specs/2026-08-30-support-match-v22-design.md
Conventions: everything from v2.0/v2.1 (0-based engine keys, TDD real RED, build-slot
protocol, off-mode purity, visited-flag idempotency, [chameleon] tag, fixtures
untouched). Suite baseline: 32 cases / 149 assertions.

## Task 1 — Cap semantics (spec C1-C3) [Print.cpp, BrimFilament path-length helper if needed, tests]

1. Delete the `switches > 3` whole-layer revert and the cumulative_switches/escalated
   machinery. partition_support_entities' return value stays (harmless) but no longer
   drives caps.
2. After the layer's engine calls: compute per-bucket total path length (reuse/expose
   the BrimFilament path-length helper). Apply, in order:
   a. Min-benefit gate: buckets < 40.0mm merged back to support_fills.
   b. Distinct-extruder trim: if > 2 buckets remain, rank by (in previous layer's kept
      set DESC, total length DESC) and merge the excess back.
   c. Commit survivors to interface_by_extruder; record this layer's kept extruder set
      for the next layer's hysteresis.
3. Merge-back = the existing ownership-transferring append (role-preserving paths land
   back in support_fills and print fallback). Verify no leak/double-free.
4. Unit tests (public engine APIs + synthetic resolvers): 3-extruder partition trims to
   the 2 longest; hysteresis flips the tie; sub-40mm bucket dropped. (Layer-loop logic
   not unit-instantiable → cover trim/gate as free functions in BrimFilament so they ARE
   testable: `apply_bucket_caps(map, prev_set, max_extruders, min_len_mm)` — implement
   there, call from Print.cpp.)

## Task 2 — Lateral + projection reach (spec C4-C5) [Print.cpp, BrimFilament, tests]

1. C4: per-object lateral cap = object->config().support_object_xy_distance +
   0.5·outer-wall width + 0.5·support line width + 0.35 slack. Pull widths the way the
   support generator computes them (grep support_line_width / extrusion width resolution
   in SupportCommon/SupportMaterial; first-layer variants ignored — document). Replaces
   the hardcoded 1.0 at the vote-params copy.
2. C5: projection views carry lslices expanded by SUPPORT_MATERIAL_MARGIN +
   object->config().support_expansion (offset_ex once per band layer at view build;
   bbox inflated). Point-in-expanded → treat as hit; region pick: containing region of
   the raw geometry first, else nearest region bbox/contains within the margin ring
   (document the chosen form). Unit tests via ProjectionLayerView with hand-built
   expanded polygons (margin-ring hit resolves; far miss still misses).
3. Full app build + suite + verify (off-mode untouched: all changes behind the
   nearest_surface gate).

## Task 3 — Ironing + nested collections (spec C6-C7) [Print.cpp, BrimFilament, GCode.cpp, ToolOrdering.cpp, tests]

1. Third engine call: partition_support_entities(..., erIroning, fallback=interface
   fallback, interface_resolver, ...) sharing map/caps (runs BEFORE Task 1's
   gate/trim, which operates on the combined map).
2. Emission: after extrude_support(bucket, erMixed) add extrude_support(bucket,
   erIroning) — verify with line evidence that erMixed excludes erIroning and the
   second call emits exactly the ironing subset; ordering ironing-last preserved.
3. ToolOrdering + the GCode support-bucket mirror: a support_fills whose collapsed
   role() is erIroning must set has_interface so residual fallback ironing registers
   and never drops. Anchors from investigation: ToolOrdering.cpp:701-703, GCode.cpp:5339-5341.
4. C7 nested collections in partition_support_entities: role-eligible nested
   collections voted as ONE unit (sample its flattened polylines with the resolver;
   majority vote; move whole collection pointer into out[extruder] or leave). Split
   paths inside collections are NOT created. Role filter: a collection is eligible if
   its collapsed role() matches role_filter. Unit tests: nested collection moves whole;
   mixed-role nested collection left alone; ironing partition role-preserved.

## Task 4 — nearest_wall mode + UI + verify (spec C8) [PrintConfig, Print.cpp, Tab.cpp, ConfigManipulation.cpp, verify script, tests]

1. Enum value `sifsNearestWall` / serialized "nearest_wall" / label "Nearest wall"
   appended to SupportInterfaceFilamentSource maps + option def enum lists + tooltip.
2. Pass gate accepts either non-manual value; per-object mode switch: nearest_wall
   builds ONE WallSampleIndex over contact-band ∪ coplanar layers' walls, resolver =
   brim_vote UNCAPPED (max_dist 0) for ALL THREE roles (no projection). All caps,
   guards, storage, emission identical.
3. Invalidation already keyed on the option (value change re-runs posSupportMaterial);
   verify print_sequence conditional invalidation covers `!= sifsManual` (it currently
   tests == sifsNearestSurface — update to != sifsManual).
4. UI: no extra wiring beyond the enum (dropdown renders from enum_labels) — confirm
   ConfigManipulation greying already covers the option.
5. Verify script: add a nearest_wall no-crash + determinism check pair (same pattern as
   nearest_surface checks). Full 3-run stability pass. Suite green.
6. Report includes the GUI A/B instructions (switch dropdown between the two modes on
   the same plate).
