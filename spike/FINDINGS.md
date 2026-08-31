# Spike Findings: Mid-Feature Extruder Switches (color-matched supports/brim)

Date: 2026-08-29 · Branch: feat/color-matched-supports · Verdict: **GO**

## Question
Can the gcode pipeline emit extruder switches at sub-feature granularity (mid-brim-loop,
mid-interface-layer) without corrupting toolpaths, flush accounting, or state?

## Verdict per risk

| Risk | Verdict | Evidence |
|---|---|---|
| R1 path emission / reordering | **PASS** | Injected splits produce exactly the expected M620 count (4 injected → 4 emitted, no interleaving); brim resumes at correct geometry after the switch (out/exp1_final.gcode:1729+) |
| R2 tool-change placement | **PASS** | Mid-brim (layer 0) and mid-interface (layer 1, print Z 0.4) both emit the full X1C AMS sequence: retract → M620 S1A → Z-lift → travel to rear chute (X70/Y265, off-part) → cut → T1 → load/purge → M621. Next layer needs NO compensating change — writer state carries (M620 count exact) |
| R3 flush placement/accounting | **PASS** (non-wipe-tower path) | Purge routed via machine change_filament_gcode at the chute; flush is matrix-driven per A→B pair, context-free |
| R4 preview / stats | **CAVEAT** | "filament used" stat ballooned (1.6m → 111m) in the absolute-E harness; CONFIG_BLOCK filament list doesn't reflect the forced extruder. Re-verify stats + preview colors with default relative-E and REAL assignments (the forced-writer hack bypasses normal used-filament bookkeeping; the real feature won't) |
| Determinism | **PASS** | Two consecutive slices byte-identical modulo the timestamp header |

## Architectural facts discovered (Part 1/2 implementation map)

1. **ExtrusionEntity carries NO extruder id** in this lineage — the research docs' premise
   ("paths carry extruder_id") is false. Extruder choice happens at gcode time:
   ToolOrdering + per-feature config scalars. ⇒ Both specs' "tag sub-paths with extruder"
   sections become "partition entities into per-extruder buckets" (the ObjectByExtruder
   pattern already used to split support base vs interface across extruders).
2. **Injection seats**: brim = per-object loop in GCode::process_layer
   ("add brim by obj by extruder", ~L6528); supports = GCode::extrude_support (role-filtered)
   + the (object,extruder)→role bucket build at ~L5333-5423. Support base and interface
   ALREADY print with different extruders on one layer — mid-layer multi-extruder support
   is routine, not novel.
3. **Used-extruder registration**: any extruder the spatial assigner picks must join the
   print's used-extruder set before GCode::set_extruders (writer needs multiple_extruders
   =true or set_extruder() takes the hollow single-extruder fast path — observed directly).
4. `m_brimMap` per-object brim is one collection entity — flatten() before splitting.
5. Single-filament supports pass role erMixed (not pure interface) through extrude_support.

## Harness notes
- CLI: resources/profiles presets load with an inherits-resolving override JSON needing
  `compatible_printers`; `--export-3mf` segfaults post-gcode (pre-existing; slice without it).
- `--load-filament-ids` did not map plain-STL objects to filaments in our runs (all objects
  sliced filament 1); per-object filament fixtures should be made in the GUI as 3mf.
  ⇒ Also means "does per-object brim already color-match for separated objects?" is
  STILL OPEN — verify in GUI before writing Part 1's scope (it may already work).
- Spike code: all under SPIKE_SPLIT_BRIM in GCode.cpp (commit 32b5ef515d + follow-ups);
  fixtures spike/cube30.stl, tshape.stl; gcode artifacts in spike/out/.

## Spec amendments required
- Replace "sub-path splitting w/ per-path extruder ids" with "entity partitioning into
  per-extruder collections + used-extruder registration" (both parts).
- Part 1 scope check: verify GUI behavior for separated multi-filament objects first.
- Keep the switch-count guard in the emitter (R1 held here, but guard stays essential).
