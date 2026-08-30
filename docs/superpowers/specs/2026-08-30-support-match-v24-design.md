# Support Filament Match v2.4 — Design Spec

Date: 2026-08-30 · Branch: feat/color-matched-supports · Status: approved direction
Amends v2.3. Driven by GUI A/B round 2: nearest_wall = the keeper but small supports
under thin painted features (white claws) print fallback/large-wall color;
nearest_surface = still wrong (layer mixing even in large single-color areas) — USER
DECISION: drop it, migrate old saves → nearest_wall; integrate its upward cast for
IRONING + TOP INTERFACES into nearest_wall later (v2.5).

## Root cause of the claw symptom (workflow-verified)

Free-set eligibility used strict z-COINCIDENCE: white claw walls exist only in the
contact band (z, z+2] ABOVE the claw's support column, so white was never "free" there —
every 5-18mm/layer white bucket faced the 12mm non-free floor and merged back to
fallback (buckets_dropped_min_benefit), while plate-wide teal committed. Worse, strict
coincidence can return EMPTY on unsynced layer grids (top-z vs print_z mismatch) making
nothing free. prev_kept never bootstraps because the first white commit never happens.
The k=1 sampler itself was proven correct — this is tier MEMBERSHIP, not the vote.

## Changes

A. **Mode drop + legacy alias (USER: migrate→nearest_wall)**
   - Enum becomes {sifsManual=0, sifsNearestWall}; delete "nearest_surface" from
     keys map / enum_values / enum_labels; tooltip rewritten for two states (drop
     "comparison mode" wording).
   - handle_legacy (PrintConfig.cpp ~7402, draft_shield precedent):
     support_interface_filament_source "nearest_surface" → "nearest_wall" (without it,
     from_string fails → silent default manual substitution).
   - Print.cpp: `!= sifsManual` gate is the whole opt-in; delete the nearest_wall_mode
     local, the else-arm (projection-first interface resolver + lateral-only base
     resolver), the gap-aware lateral-cap block (pure fn + tests stay in BrimFilament),
     coplanar_wall_idx; un-nest the nearest_wall branch; log mode constant.
   - KEEP all projection machinery (ProjectionLayerView, view build, pick fn,
     band selectors) + tests as v2.5 upward-cast scaffolding, marked as such.

B. **Claw fix — windowed free-extruder query**
   - chameleon_layer_free_extruders gains (down_mm=0, up_mm=0): union all table
     entries with z ∈ [query_z − down_mm − EPS, query_z + up_mm + EPS]. Defaults
     preserve strict behavior (existing tests unchanged).
   - Call site: down = support_layer->height (interval-overlap correction — unsynced
     grids), up = kContactBandMm (2.0, hoisted constant shared with
     select_contact_layers) — free-eligibility now covers exactly the band the vote
     samples. A color the sampler can vote is a color the gate must not floor at 12mm.
   - Gate floors/max_extruders unchanged (12.0 / 3.0 / 2): tier membership fix only.
     3mm single free tier (decision: no 6mm mid-tier — claw tips carry ~5mm buckets).

C. **Diagnostics**: per-object log line gains fallback= / base_fallback= resolved ids
   (decides gate-vs-fallback-identity if any residual symptom); optionally split
   buckets_dropped_min_benefit into free/normal tier counters.

D. **Tests/harness**: windowed-query unit cases (down rescue, up rescue, zero-window
   parity); verify script retargets nearest_surface checks → nearest_wall + adds a
   legacy-alias check (old key slices as nearest_wall, no substitution notice).

E. **Non-changes (scoped out, documented)**: purge-into-support residual paint stays
   (waste saver; residual is deep-column don't-care post-fix); min_run 1.6 / max_runs 8
   stay; k=1/max_dist=0 vote untouched; bottom-contact matching remains v2.4+ backlog
   (unchanged user deferral); v2.5 = upward cast for ironing/top interfaces inside
   nearest_wall.

## Testing

Unit baseline 92/430 + windowed-query cases; verify script (retargeted) 30-check level
stable ×2; off-mode purity; legacy-alias slice check. GUI: claws' supports should match
white where claw walls are in-band; large areas stay correct; old mcsupport project
(saved with nearest_surface) loads as Nearest wall.
