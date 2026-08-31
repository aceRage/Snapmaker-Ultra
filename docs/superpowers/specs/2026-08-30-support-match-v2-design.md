# Support Filament Match v2.1 (projection + lateral proximity) — Design Spec

Date: 2026-08-30 · Branch: feat/color-matched-supports · Status: approved direction
Supersedes the matching ALGORITHM of 2026-08-29-support-interface-match-design.md;
all pipeline architecture (pass placement, storage, ToolOrdering registration, emission
site, guards, idempotency, invalidation) carries over unchanged.

## Why (GUI findings, forensics-confirmed on tests/mcsupport_test.3mf + .gcode)

1. **Mismatch bug (CONFIRMED)**: v2.0 votes by 2D-XY-nearest wall over ALL walls in the
   z-band — no XY relation to what the support actually touches. Adjacent painted
   regions/features outcompete the supported surface; empty `object_area` makes near-ties
   resolve to min extruder id (white T0 won 6/9 provably-wrong cases); band-shift between
   consecutive interface layers flips the same column between tools (9 smoking-gun pairs
   at identical XY, e.g. z 28.92 T2 → z 29.04 T3).
2. **User scope change**: ANY support material (base included) within 1 mm of a wall at
   its own layer must match that wall (brim-like anti-contamination rule).
3. UI: default enum label renamed "Manual" → "Default" (serialized key `manual` kept).

## Decision rules (user-approved)

- **Interface entities — vertical projection is primary**: each 0.8 mm sample point p
  takes the filament of the model surface directly above it: the first (lowest) object
  layer in (support_top_z, support_top_z + 2.0 mm] whose `lslices` cover p; within it,
  the LayerRegion whose `slices` contain p (prefer regions with stBottom/stBottomBridge
  `fill_surfaces` covering p); resolved via the Part 1 wall-filament rules
  (outer_wall_filament → wall_filament → object default). Projection wins over lateral
  whenever it hits ("surface above wins" — user decision; a future config option for the
  opposite priority is FENCED, design the resolver so priority is one branch).
  **v2.1 fix-wave correction (I3, 2026-08-30):** the projected surface is the model's
  bottom shell, which the pipeline prints with `solid_infill_filament`, not
  `wall_filament` — the projection branch resolves `solid_infill_filament` →
  `sparse_infill_filament` → object default instead; the wall-filament chain above
  stays exactly as written for the lateral (wall-sample) rule below.
- **Lateral rule — all roles**: any sample point (interface points where projection
  misses, and ALL base/erSupportMaterial points) matches the nearest wall of the
  COPLANAR object layer(s) (z-interval overlap with the support layer's own span) iff
  that nearest wall is ≤ 1.0 mm away; otherwise fallback extruder. Implemented as a
  `max_dist_mm` cap in BrimVoteParams (engine currently has NO distance cap).
- **Fallback** unchanged: resolved support_interface_filament (interfaces) /
  support_filament scalar path (base — mirror ToolOrdering's base scalar the same
  approximate way v2.0 mirrors the interface scalar).

## Engine changes (BrimFilament)

- `partition_support_interfaces` generalized: caller-supplied role filter (interface
  pass and base pass share one engine); emitted split paths COPY the source entity's
  role (stop hardcoding erSupportMaterialInterface) so base runs stay erSupportMaterial
  for downstream speed/flow handling.
- Per-point resolver: run-splitting (`split_polyline_by_vote` machinery — absorb,
  min_run, max_runs, boundary-sharing) is retained but fed by a per-point
  `std::function<unsigned(const Point&)>` resolver instead of the raw knn vote, so
  projection-then-lateral composition lives outside the engine.
- `BrimVoteParams.max_dist_mm` (0 = uncapped, Part 1 brim unchanged) applied to lateral
  knn votes.

## Pass changes (Print.cpp)

- Interface resolver = projection (band layers' lslices/regions, bbox-gated
  point-in-ExPolygon) → lateral (coplanar WallSampleIndex, 1 mm cap) → fallback.
- Base resolver = lateral (1 mm cap) → fallback. Base entities partitioned with the same
  engine + role filter; results share `interface_by_extruder` (keyed by extruder; paths
  carry true roles). Base partitioning obeys the same plate guard, visited flag, caps.
- Caps stay 3 switches/layer, 20/object, now shared across both roles (raise later if
  GUI shows base-heavy plates starve interfaces; constants, not config).
- Coplanar layer selection: generalize `select_contact_layers` to a (lo, hi] band form
  or add a sibling `select_coplanar_layers`.

## Emission (GCode.cpp)

Emission block unchanged in placement; the `extrude_support` role argument must let each
path's own role drive config now that the map can hold mixed roles — pass erMixed
(existing single-filament precedent) and verify speed/flow selection per role.

## Stability

Projection kills both aggravators for interfaces (fixed surface above a column ⇒ both
top-interface layers agree; no tie-break dependence). Lateral votes keep engine
tie-breaks but the 1 mm cap bounds the damage radius. Determinism requirements unchanged.

## Testing

- Unit ([chameleon]): resolver composition (projection hit / lateral hit / both miss);
  role-preserving splits (base stays base); max_dist cap; coplanar selection; base
  entities partitioned + interfaces still correct on mixed support_fills.
- Integration: verify script re-run (off-mode identity vs same baseline — off path
  untouched); mcsupport fixture GUI re-slice by user: the 9 flip-flop column pairs must
  print both top-interface layers in the SAME tool, and near-wall base must match walls.
