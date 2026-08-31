# Support Interface Auto-Match (Part 2, v1) — Design Spec

Date: 2026-08-29 · Branch: feat/color-matched-supports · Status: approved design
Sources: C:\Dev\supportextension.md (research), Part 1 spec + FINDINGS + the three
Part-1-validated corrections (entity partitioning not tagging; extruder registration must
precede wipe-tower planning; run-stable ordinals in every ordering).

## Purpose

When `support_interface_filament_source = nearest_surface`, each support-interface
extrusion is assigned the filament of the model surface it will touch (walls of the one
or two layers above it), per contact region — so a PETG-and-TPU part gets PETG interface
under its PETG overhangs and TPU interface under its TPU overhangs, instead of one scalar
choice for the whole object.

## Scope (v1) and non-goals

In: normal (non-tree) support interfaces, per-layer contact matching, guard caps,
plate-adhesion guard, opt-in config, off-mode byte-identity, determinism.
Out (fenced, v1.1+): support BASE matching + column inheritance; tree/organic interface
tips (branch-uniformity rule); soluble-interface warnings; configurable gap/caps;
`auto_match_scope` (implied interface_only in v1).

## Config surface

- `support_interface_filament_source`: coEnum [`manual` (default) | `nearest_surface`],
  object/process support section, comAdvanced. `manual` = today's behavior, untouched.
- Constants (promote to config only on demand): match gap 2.0 mm; guard caps 3
  extruder-switches per interface layer, 20 per object; sample spacing 0.8 mm; grid 2 mm;
  vote k=3 inverse-square with Part 1's tie-breaks.

## Architecture (pipeline-ordering driven)

Pipeline fact this design exploits: `make_perimeters` and `generate_support_material`
(object steps, Print.cpp ~2511/2555) complete before `psWipeTower` builds ToolOrdering —
so assignment can finish before toolchange/wipe-tower planning, and interface extruders
get planned purge slots (the failure Part 1 hit in reverse cannot occur).

| Unit | One purpose | Depends on |
|---|---|---|
| Contact index build (new fn beside the Part 1 pass) | For support layer L of an object: WallSampleIndex over wall paths (external/internal perimeter roles) of object layers whose z-range overlaps [L.top, L.top + gap], instance-shifted, region-filament-tagged, stable-ordinal object keys | WallSampleIndex (Part 1) |
| Interface partition pass (`Print::process`, new call after object steps, before psWipeTower) | Per object, per support layer: split `support_fills`' erSupportMaterialInterface entities via brim engine votes into per-extruder collections; guard-coalesce to caps; store results | BrimFilament engine (Part 1) |
| Storage | `SupportLayer` gains `std::map<unsigned, ExtrusionEntityCollection> interface_by_extruder;` (empty = feature off / nothing matched — all legacy paths unchanged) plus the matched entities REMOVED from `support_fills` (Part 1 lesson: never leave partitioned entities where a legacy print site will emit them under an arbitrary extruder) | — |
| ToolOrdering registration | In the existing per-support-layer block (ToolOrdering.cpp ~700): union `interface_by_extruder` keys into that layer's extruders (order-preserving append; ctor-time is CORRECT here because the pass ran earlier) | storage |
| GCode emission | In the (object, extruder) support bucket build (GCode.cpp ~5333-5423): for each assigned extruder, add an interface bucket carrying its partition; `extrude_support` receives the pre-partitioned collection (no role-refiltering); prints inside the normal by-extruder loop, after the toolchange | storage |

## Algorithms

- **Contact window**: interface at layer L matches walls in z-window (L.print_z,
  L.print_z + 2.0 mm], i.e. typically the 1–2 object layers above (docs' L+1..L+2).
  Implementation keys object layers by print_z overlap, not index arithmetic (VLH-safe).
- **Vote/split/guard**: exactly Part 1's `brim_vote` + `split_polyline_by_vote` with
  per-layer params: fallback = the object's resolved interface extruder (the same value
  ToolOrdering's scalar path computes); guard merges smallest runs until ≤ 3 switch
  boundaries per layer; if an object's total assigned switches exceed 20, remaining
  layers degrade to majority-per-layer (docs' escalation).
- **Plate guard**: interfaces on a support layer touching the plate (first support layer,
  print_z ≈ first layer height) always keep the fallback extruder.
- **Zero-sample layers**: no wall samples in window → whole layer keeps fallback
  (no warning spam; one summary log line per object).
- **Determinism**: object ordinals for keys/areas (Part 1 lesson); stable iteration
  (objects vector order, layer order, sorted extruder maps); byte-identical reslices.
- **Off-mode purity**: `manual` or single extruder or ByObject sequence ⇒ the pass never
  runs, `interface_by_extruder` stays empty, `support_fills` untouched — byte-identical
  gcode (Part 1's verify-script pattern extended to a support fixture).

## Error handling

Partition failure/degenerate entities → leave that entity in `support_fills` (legacy
path). ToolOrdering unavailable cannot occur (pass precedes it); assert in debug.
Interface entity types: paths/multipaths/loops handled as in Part 1; unknown entity
types left untouched in `support_fills`.

## Testing

- Unit ([chameleon] tag continues): contact-window layer selection (incl. unequal layer
  heights + z-gap edge); interface partition on synthetic support_fills (mixed roles —
  base must never be touched); plate guard; per-layer/per-object caps; determinism
  (two runs identical).
- Integration: T-shape fixture (spike/tshape.stl) sliced with wipe tower ON and OFF —
  off-mode byte-identity vs a fresh baseline; nearest_surface mode: exit 0, bounded
  M620 growth, `; FEATURE: Support interface` present.
- GUI validation (user): multi-material overhang plate fixture saved as project 3mf;
  interface colors match the surfaces above; order-independence across first-layer
  sequences. Then physical print.

## Increments

1. Contact-index builder + unit tests (pure, reuses WallSampleIndex).
2. Interface partition pass + SupportLayer storage + config key + unit tests.
3. ToolOrdering registration + GCode bucket/emission + integration slices (tower on/off).
4. Verify-script extension (off-mode identity + determinism for supports) + GUI handoff.
