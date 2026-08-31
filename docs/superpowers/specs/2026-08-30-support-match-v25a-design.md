# Support Filament Match v2.5a — Residual paint fix (design)

Date: 2026-08-30 · Branch: feat/color-matched-supports · Status: approved direction
Symptom (GUI round 3): khaki randomly mixed into strictly white/teal support areas with
no khaki walls nearby, tree AND normal. Workflow-confirmed mechanisms — matched runs are
SAFE by construction (removed from support_fills; dedicated emission never consults
WipingExtrusions); the khaki is the RESIDUAL path:

A. Dominant: flush_into_support (default ON, PrintConfig.cpp:6254-6260) — per qualifying
   toolchange, mark_wiping_extrusions (ToolOrdering.cpp ~1583-1618) claims the object's
   ENTIRE residual support_fills for that layer and repaints it the purge target color.
   Matched mode adds toolchanges per interface bucket → amplifies its own repainting.
B. Secondary: on unclaimed layers the don't-care scalar path resolves residual support
   to the layer's first/active non-soluble extruder — khaki whenever khaki leads.

## Changes (per the workflow's implementation-ready plan)

1. RESIDUAL PIN (lead): is_support_overriddable (ToolOrdering.cpp:1468) early-returns
   false when object.config().support_interface_filament_source != sifsManual; refactor
   mark_wiping_extrusions' support branch (~1583-1618, which checks the scalar configs
   INLINE) to call the predicate so the two sites can never drift. No new config knob
   (mode is opt-in; soluble suppression is precedent).
2. REDIRECT: apply_bucket_caps — gate/trim drops go to a local dropped list; after the
   kept set is final, redirect each dropped bucket to the surviving bucket with min
   squared centroid distance (snapshot survivor centroids BEFORE any append; scaled-int
   math; ties prev_kept DESC then extruder ASC; gate entries first then trim, ascending
   src extruder). Map empty → legacy merge_back_target append (byte-identical old
   behavior; residual pin owns that layer). No caller signature change; kept/
   distinct-extruder/max_extruders/prev_kept/retention semantics UNTOUCHED (no new
   toolchanges). New BucketCapResult.buckets_redirected + log field.
3. MATCHED-RUN GUARD: comments (+optional debug assert) documenting that matched
   buckets never flow through WipingExtrusions/support_map.
4. Tooltips: flush_into_support notes it is ignored for mode-active objects; matching
   note on support_interface_filament_source.
5. Tests: redirect-to-nearest, centroid tie-break, no-survivor legacy fallback,
   permuted-insertion determinism, counter; off-mode purity re-check; mode-active
   "support_map stays empty" check where expressible.

Known remaining gap (named, not claimed fixed): wholly-fallback/no-survivor layers
still follow the active-extruder rule (deterministic per run, layer-varying).
Accepted cost: purge moves to infill/objects/tower for mode-active objects (tens of
grams on support-heavy prints). Risk: redirected bucket prints a clean adjacent-but-
wrong color; bbox centroid crude for L-shapes — nearest-centroid minimizes.
