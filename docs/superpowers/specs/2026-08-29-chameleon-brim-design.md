# Chameleon Brim (Part 1) — Design Spec

Date: 2026-08-29 · Branch: feat/color-matched-supports · Status: approved design
Sources: C:\Dev\multicolorbrim.md (research), spike/FINDINGS.md (verdict GO + architecture
corrections). Part 2 (support interface auto-match, C:\Dev\supportextension.md) follows on
this branch and reuses the WallSampleIndex; this spec covers brim only.

## Purpose

When `brim_filament_source = nearest_wall`, every brim extrusion is assigned to the
filament of the nearest layer-0 wall, so the brim is visually and materially continuous
with what it touches. Fixes the two cases broken today: merged/adjacent multi-object brims
(currently divided by arbitrary clipping precedence) and multi-color single objects
(brim rides one filament). Separated single-color objects already match (verified:
Brim.cpp per-object-per-extruder generation) and must keep byte-identical output when the
feature is off.

## Non-goals (fenced)

Blend-zone dithering (phase 2); per-material brim width/separation geometry; support-brim
and raft filament logic (Part 2 territory); wipe-tower brim (always wipe-tower filament);
any change when `brim_filament_source = object` (default) or on single-extruder prints.

## Config surface

- `brim_filament_source`: coEnum [`object` (default) | `nearest_wall`], process config,
  brim section, comAdvanced. Serializes into gcode CONFIG_BLOCK + 3mf for free.
- No other new keys. Switch-count guard cap is a constant (4 runs per object brim);
  promote to config only if real use demands it.

## Architecture (spike-corrected)

Key correction from the spike: ExtrusionEntities carry NO extruder id in this lineage —
assignment = partitioning entities into per-extruder collections + registering those
extruders as used. Both mechanisms already exist for support base/interface splits.

| Unit | One purpose | Depends on |
|---|---|---|
| `WallSampleIndex` (`src/libslic3r/WallSampleIndex.{hpp,cpp}`) | Layer-generic spatial index: 2 mm uniform grid over sampled wall points `(x, y, extruder_id, object_id)`; k-NN queries | libslic3r geometry only |
| Brim assigner (inside `Brim.cpp`) | Sample brim loops, vote, split into runs, coalesce (guard), emit per-(object, extruder) collections | WallSampleIndex |
| Brim storage | `Print::m_brimMap` value becomes per-extruder: `std::map<ObjectID, std::map<unsigned, ExtrusionEntityCollection>>` (or a parallel `m_brimMapByExtruder` if touching m_brimMap ripples too far — decide in plan against call sites) | — |
| ToolOrdering hook | Brim-assigned extruders join layer-0 `LayerTools.extruders` so the writer registers them (else `set_extruder` takes the hollow single-extruder fast path — spike-observed) | assignment results |
| GCode emission | The per-object brim block in `GCode::process_layer` (~L6528) prints only the current extruder's partition for that object | storage |

## Algorithms & constants

**Wall sampling (layer 0):** for every object's layer-0 wall extrusion paths (roles
erExternalPerimeter, erPerimeter, erOverhangPerimeter), sample points every ~0.8 mm with
the region's wall filament id (outer wall filament for external loops, wall filament
otherwise, honoring the fork's `outer_wall_filament` split). Grid cell 2 mm.

**Vote (per brim sample point, ~0.8 mm spacing along each brim path):** k=3 nearest wall
points, inverse-square weight (1/d²), winner takes the point. Tie-break when the top two
extruders' weighted scores differ by < 30% or nearest distances differ by < 0.3 mm:
(1) object with larger layer-0 area wins; (2) lower extruder id. All comparisons on
deterministically ordered candidates (sort by (distance, extruder_id, object_id)).

**Run splitting:** consecutive same-extruder sample runs along a loop → split the loop's
polyline at run boundaries into open ExtrusionPaths (loops become path sequences; the
spike confirmed downstream handles path-level brim entities). Runs shorter than 2 mm are
absorbed into the neighboring run before splitting.

**Guard:** if an object's brim partition ends up with more than 4 runs, iteratively merge
the smallest run into its dominant neighbor until ≤ 4. Guarantees bounded tool changes.

**No-op paths:** single configured extruder, or `object` mode, or all votes identical →
exactly today's entities and today's gcode (bit-identical requirement).

**Fallbacks:** object with zero layer-0 wall samples (e.g. first layer all support) →
that object's brim keeps its object filament, log a warning. Wipe-tower brim excluded
from assignment entirely.

## Data flow

`Print::process()` → after layer-0 slicing, before `make_brim`: build WallSampleIndex
(only when nearest_wall + >1 configured extruder). `make_brim` produces geometry as today;
the assigner then partitions each object's brim collection. ToolOrdering (or the layer-0
LayerTools finalization) unions in the assigned extruders. GCode prints partitions under
their extruders inside the existing per-extruder × per-object loops; the AMS change
sequence, off-part flush, and preview colors come free (spike-proven).

## Corner cases (from research doc, kept binding)

- Equidistant shared brim → deterministic tie-break (documented as arbitrary but stable).
- brim_type outer_only plate-wide brim → pointwise assignment shines; same code path.
- Objects with different first-layer heights → absent walls contribute no samples; their
  region follows whatever layer-0 walls exist. Correct by construction.
- Support brim (`m_supportBrimMap`) keeps existing logic (Part 2 will revisit).

## Testing

- Unit (Catch2, `tests/libslic3r/test_wall_sample_index.cpp` + assigner tests): grid k-NN
  correctness; vote + tie-break determinism (equidistant case twice → same winner); run
  splitting on a synthetic square loop with two wall clusters → exactly 2 runs at expected
  boundaries ±0.5 mm; guard merges 6 runs → 4; sub-2mm run absorption.
- Integration (spike CLI harness, two-cube merged fixture): `nearest_wall` on → M620 count
  = expected boundaries only; off → byte-identical to pre-feature baseline gcode.
- Determinism: two slices byte-identical (harness from spike).
- Manual/physical: GUI preview two-color brim screenshot (announcement artifact); AMS
  print on the X1C.

## Increments

1. WallSampleIndex + unit tests.
2. Vote/split/guard as pure functions + unit tests.
3. Brim.cpp integration + storage + config key (feature compiled but default-off).
4. ToolOrdering/used-extruder hook + GCode emission + CLI-harness integration test.
5. Baseline-equality regression (off-mode byte-identical) + GUI/manual validation.
