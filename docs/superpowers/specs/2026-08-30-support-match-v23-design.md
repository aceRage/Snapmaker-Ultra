# Support Filament Match v2.3 — Design Spec

Date: 2026-08-30 · Branch: feat/color-matched-supports · Status: approved direction
Amends v2.2. Driven by the user's GUI A/B (nearest_surface: constant layer alternation +
wrong colors near walls + interface failures; nearest_wall: much closer but oversteers
past sector boundaries "like a minimum length"; tree: foreign filament on sector-
straddling rings). Workflow-verified root causes, code-anchored.

## Root causes (verified at 9cf6a85191)

1. **40mm min-benefit gate dominates both modes**: branch-scale buckets are 5-30mm
   (ring arcs) / 20-60mm (small interfaces) per layer — the gate merges legitimate
   buckets to fallback EVERY layer with no memory. Produces nearest_wall's
   "minimum length" oversteer, tower matched/fallback striping, and gated interfaces.
2. **v2.2 hysteresis structurally inert**: prev_kept preference only runs inside the
   trim branch (map.size() > 2 — rare); prev_kept overwritten unconditionally even to
   empty, so one all-gated layer erases column memory.
3. **Tie path low-id bias**: object_area empty ⇒ ties → std::min extruder; 0.8mm
   sample phase vs 0.3mm tie window ⇒ edge alternation.
4. **PASS 2 (margin ring) scans lowest-first**: the 1.2mm-grown contact rim resolves
   against the adjacent WALL layer below the overhang layer — inverts surface-above-wins
   on rims near walls.
5. **Tree rings spanning sectors**: whole-collection votes paint the minority arc the
   majority color — structural, not tunable.
6. **Config-read defects**: painted regions ALWAYS set wall/solid/sparse filament
   (min=1, never 0 — PrintApply.cpp:826-833/1070-1094), so the 0-value fallbacks are
   dead code AND raw solid_infill_filament reads miss the sparse_infill_density==100%
   redirect (PrintRegion.cpp:10-13). Painted-region chain otherwise INTACT.
7. **Bottom contacts (support standing ON model) can never match** — band looks only
   up. USER DECISION: deferred to v2.4; document as known limitation.
8. Ring seam double-counts a sector (first+last runs same color) inflating run counts
   into guard_max_runs; max_runs=4 trips on 4-sector rings.

## Changes (user-approved where marked)

C1. **Gate rework + free-extruder refinement (USER: ship)**: min_len 40→12mm for
    extruders NOT already printing on the layer; **3mm** for FREE extruders (already
    present at that z anywhere on the plate — their toolchange costs nothing). Free set:
    once-per-pass z-sorted table of wall/solid/sparse filaments over all objects'
    layers (ToolOrdering::collect_extruders mirror), EPSILON-merged, binary-searched
    per support layer (ByLayer z-coincidence). Empty/skip when mixed-filament gradient
    features are active.
C2. **Gate/trim hysteresis**: bucket whose extruder ∈ prev_kept passes the gate at
    0.5×eff_min; prev_kept no longer reset to empty by an all-gated layer (one-layer
    retention, counted decay — not indefinite).
C3. **Vote-level hysteresis**: BrimVoteParams gains prev_kept (by value); tie path
    prefers a prev_kept member (between the tie test and object_area/min-id fallbacks);
    also applied to whole-collection majority ties. k=1 nearest_wall path unaffected
    (short-circuits pre-tie). No widening of tie windows.
C4. **PASS 2 scan order**: margin-ring pass scans HIGHEST band layer first (overhang
    ring beats wall-gap ring for rim samples) — restores surface-above-wins on rims.
C5. **Tree selective descent (USER: selective)**: mixed-vote collections descend
    (leaves partitioned individually, split paths per-leaf flow attrs) only when the
    minority's contiguous run ≥ min_run_mm; uniform/dominant collections whole-move as
    today; all-fallback keeps pointer. Day-one requirements: propagate can_reverse()
    onto rebuilt runs (seam-anchor blob hazard, GCode chain_and_reorder); splice
    fallback runs at the source leaf's position in no_sort collections; delete only
    emptied shells; per-column descend hysteresis (halved threshold if the column
    descended last layer).
C6. **Param overrides (support pass only — brim untouched)**: max_runs 4→8;
    min_run_mm 2.0→1.6; sample_mm stays 0.8; max_extruders stays 2.
C7. **Seam-run merge**: for ring paths, merge first+last runs when same extruder
    BEFORE absorb/guard (kills the double-counted seam sector).
C8. **Config-read fix**: projection/lateral region reads go through
    PrintRegion::extruder(frSolidInfill/frExternalPerimeter etc.) semantics (handles
    the 100%-density sparse redirect); remove unreachable 0-value fallbacks.
C9. **Validation gates**: log-counter readout on a failing slice (gate/trim counters vs
    interface_runs_matched); gcode emission discriminator (per-layer partition blocks vs
    expected painted extruder) before blaming resolvers; bottom-contact check in any
    remaining failure triage (deferred limitation).

## Testing

Unit ([chameleon], baseline 60/265): gate tiers (free 3 / prev_kept half / normal 12);
prev_kept retention across an all-gated layer; tie-prefers-prev_kept; PASS 2
highest-first cross-layer case; selective-descent threshold + can_reverse propagation +
no_sort splice position; seam-run merge; PrintRegion-semantics extruder reads (100%
density redirect). Verify script: 30/30 + off-mode byte-identity (all changes gated).
GUI: re-test both modes on mcsupport_test.3mf; expect nearest_wall sector-faithful and
nearest_surface stable columns; supports-on-model still fallback (known limitation).
