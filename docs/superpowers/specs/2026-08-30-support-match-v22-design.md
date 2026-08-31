# Support Filament Match v2.2 — Design Spec

Date: 2026-08-30 · Branch: feat/color-matched-supports · Status: approved direction
Amends v2.1 (2026-08-30-support-match-v2-design.md). Root causes from the GUI test
(khaki-along-teal-wall + khaki-under-white-nail) — workflow-investigated, code-anchored.

## Root causes being fixed

1. **Lateral cap unsatisfiable**: 1.0mm cap is centerline-to-centerline; physical
   minimum wall↔support distance = support_object_xy_distance + half outer-wall width +
   half support line width (~0.75mm best case, > 1mm at common gap_xy) → nearly all
   base/lateral votes fall back.
2. **Fallback = khaki**: support/interface filament configs of 0 resolve through
   chameleon_object_default_extruder = layer-0 MIN external-perimeter extruder — the
   khaki region. Every fallback paints khaki.
3. **Cap measures the wrong thing**: support fills are one ExtrusionPath per LINE
   (link_max_length_factor=0), so 2-3 boundary-crossing lines exceed the 3-switch cap →
   whole-layer revert; committed layers burn the 20/object budget in ~7-11 layers →
   escalation locks fallback from that z upward. TRUE per-layer cost = number of
   DISTINCT matched extruders (emission groups all runs per extruder; run count is
   irrelevant to toolchanges).
4. **Projection miss on small features**: interface contact polygons are grown up to
   SUPPORT_MATERIAL_MARGIN (1.2mm) + support_expansion beyond the overhang, so samples
   over small features (the nail) land outside lslices → lateral/fallback khaki.
5. **Ironing uncovered**: erIroning entities in support_fills are never partitioned and
   the erMixed emission branch EXCLUDES erIroning — matched interface tops get repainted
   with the scalar filament (and matched ironing would silently drop if naively mapped).
6. **Nested collections skipped**: tree double-wall branch collections, layer-0 sheath
   no_sort collections, tree Roof1stLayer — all invisible to the matcher.

## Changes (decision rules)

C1. **Per-layer cap → distinct-extruder cap with partial degradation**: after
    partitioning a layer, if the partition map has > 2 extruders, keep the 2 buckets
    with the largest total path length and merge the rest back to support_fills
    (fallback), NOT whole-layer revert. Hysteresis: when trimming, prefer extruders
    kept on the PREVIOUS support layer (stability up the column) before length.
C2. **Remove the 20/object escalation** entirely (accumulating a per-layer cost proxy
    measures nothing physical).
C3. **Minimum-benefit gate**: drop (merge to fallback) any bucket whose total matched
    path length < 40mm — matched slivers aren't worth a toolchange/purge.
C4. **Gap-aware lateral cap**: max_dist_mm = support_object_xy_distance + 0.5·outer
    wall width + 0.5·support line width + 0.35mm slack, computed per object (replaces
    hardcoded 1.0). The intent stays "the support skin facing a wall matches it".
C5. **Projection margin**: test samples against band-layer lslices EXPANDED by
    SUPPORT_MATERIAL_MARGIN + support_expansion (precomputed once per band layer in the
    hoisted view build; bbox gate inflated identically). Region pick unchanged (nearest
    containing region of the unexpanded geometry; for points only inside the expansion
    ring, use the nearest region of that layer within the margin — implementer picks the
    cheapest correct form and documents it).
C6. **Ironing follows its interface**: partition erIroning with the interface resolver
    (third engine call, shared map/caps); emission adds a second
    extrude_support(bucket, erIroning) call (erMixed excludes ironing); ToolOrdering
    treats a support_fills whose collapsed role is erIroning as has_interface so
    residual fallback ironing still registers its extruder.
C7. **Nested collections**: vote each nested collection as ONE unit (majority/first-path
    sample vote via its flattened points; move the whole collection to the winning
    bucket or leave in place for fallback) — no per-path splitting inside collections.
C8. **NEW MODE (user directive): `nearest_wall`** — third enum value
    [manual|nearest_surface|nearest_wall], UI label "Nearest wall". Resolver for ALL
    roles (interface, base, ironing) = pure nearest-wall vote over the v2.0-style
    CONTACT-BAND walls UNION the coplanar walls, UNCAPPED (max_dist 0) — nearest wall
    segment wins outright, no projection. Shares everything else: guards, caps (C1-C3),
    storage, emission, invalidation (add the enum value to the same invalidation lists).
    Purpose: A/B comparison against nearest_surface.

Fallback derivation (layer-0 min extruder) is left as-is this round — C4+C5 make
fallback rare; a local-context default is DEFERRED.

## Testing

Unit ([chameleon], suite currently 32/149): distinct-extruder trim keeps 2 longest +
hysteresis preference; min-benefit gate; gap-aware cap arithmetic; expanded projection
hit in the margin ring; ironing partition + role preservation; nested-collection unit
vote; nearest_wall resolver picks nearest regardless of distance. Off-mode byte-purity
re-verified (verify script 22/22). GUI re-test: teal-facing support skin matches teal;
nail interface white; ironing matches; then A/B nearest_wall vs nearest_surface.
