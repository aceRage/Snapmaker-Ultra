# Slice Compare — Design Spec

Date: 2026-08-29 · Status: approved design, pre-implementation
Origin: user prototype notes (`C:\Dev\slicediff.md`) adapted to in-app C++.

## Purpose

Let a user compare two sliced results **visually and numerically** so they can see and
understand what changed: settings deltas, time/filament per feature, and where on each
layer the toolpaths actually differ. Two entry modes:

1. **Session A/B** — compare any two slices made this session (any plates, any presets).
2. **File mode** — compare two already-sliced files (`.gcode` / `gcode.3mf`), including
   files from other slicer versions or other slicers (config block travels in the file).

Non-goals (v1): cross-session persistence of snapshots; 3D overlay in the main preview;
feature-colored overlay mode; arc (G2/G3) expansion; comparing >2 slices.

## Architecture

New, self-contained module `src/slic3r/GUI/SliceCompare/` (+ one libslic3r-side data
builder) with four units:

| Unit | One purpose | Depends on |
|---|---|---|
| `SliceSnapshot` (data) | Compact, immutable capture of one sliced result | `GCodeProcessorResult` |
| `SnapshotStore` | Session ring buffer (8) of snapshots + labels | SliceSnapshot |
| `SliceDiff` (engine) | Pure functions: config diff, feature stats, Z-matched layer diff, segment set-diff | 2× SliceSnapshot |
| `SliceCompareFrame` (UI) | Non-modal frame: pickers, tables, layer slider, overlay/side-by-side canvas | SnapshotStore, SliceDiff |

The engine is UI-free and unit-testable; the UI renders engine output only.

## Data model — `SliceSnapshot`

Built from `GCodeProcessorResult` (NOT from re-parsing gcode text — the app's own
numbers, exact same semantics in both entry modes):

- `label` (plate # + printer/preset + timestamp), `source` (session | file path)
- `config`: `std::map<std::string,std::string>` — serialized keys from the result's
  full config (session: `full_print_config`; file: parsed CONFIG_BLOCK, which the
  existing loader already recovers)
- totals: estimated seconds, filament mm + grams (per-extruder diameter/density from
  `filament_diameters`/`filament_densities` — no hardcoded PLA), layer count, max speed
- `layers`: ordered map keyed by **quantized Z (10 µm)** → `LayerRec`
  - `feature_seconds[ExtrusionRole]`, `extrusion_mm`
  - `cells`: 10 mm-grid deposition fingerprint (`std::set<std::pair<int16,int16>>`)
  - `bbox`
  - `segs`: extrusion segments, int-packed (`x0,y0,x1,y1` quantized to 10 µm as
    `int32`, + role id) — ~8 MB per 6-hour print, acceptable for a ring of 8;
    ring eviction frees oldest
- Built by walking `GCodeProcessorResult::moves` (`MoveVertex`: type, position, delta_e,
  time, role); only `EMoveType::Extrude` contributes segments/cells.

Capture hook: `Plater::priv::on_process_completed` (Plater.cpp ~10380) on success →
`SnapshotStore::add(build_snapshot(result, label))`.

File mode: run the existing external-gcode load path (`Plater::load_gcode` machinery /
`GCodeProcessor::process_file`) **headlessly into a local Result** (no preview switch),
then the same `build_snapshot`.

## Diff engine

- **Config diff**: union of keys, rows where values differ / present in one only.
  Noise filter: ignore volatile keys (`print_host*`, timestamps, filename-ish).
- **Feature stats**: per `ExtrusionRole` — seconds, grams, path length for A, B, Δ.
- **Layer matching**: greedy two-pointer over Z-sorted layers, `Z_TOL = 0.05 mm`;
  unmatched layers reported honestly as A-only / B-only (unequal layer heights ⇒
  aggregate + config diff carry the story; never interpolate).
- **Per-layer flags** (matched layers): cell-Jaccard overlap; classify
  `RELOCATED` (overlap < 0.5, |Δe| small), `GEOMETRY-CHANGED` (bbox delta > 5 mm),
  `SUPPORT-CHANGED` (support seconds delta), `MATERIAL-ADDED-NEW-REGION`.
- **Segment diff** (for the canvas, computed per displayed layer, lazily):
  quantized direction-insensitive keys (endpoints canonically ordered) →
  set-intersection = `both`; then **proximity rescue** — A-only segments with a B-only
  counterpart of similar length whose midpoint is within 0.3 mm (coarse 1 mm grid
  lookup) are reclassified `jitter`. Output classes: both / a_only / b_only / jitter.
- **Biggest change**: matched layer maximizing |Δtime| (ties: |Δe|) — drives the
  "jump to biggest change" button; changed-layer list drives slider markers.

## UI — `SliceCompareFrame`

Non-modal, resizable; opened from **Tools → Compare Slices…** and a
**Compare with previous slice** shortcut (preselects last two snapshots of the
current plate, falling back to last two of the session).

- Top bar: A and B pickers — dropdown of session snapshots + "Browse file…"; swap A/B.
- Header strip: est. time, filament g, layers, max speed (A / B / Δ).
- Left panel (tabs): **Settings changes** (config-diff grid) · **By feature** (stats grid).
- Main canvas (`wxGLCanvas`, 2D ortho in mm, zoom/pan, Y up):
  - **Overlay** (default): both = gray 35 % opacity, A-only = blue, B-only = red,
    jitter = faint green; diff classes drawn on top.
  - **Side-by-side** toggle: A | B panes, shared camera + shared layer slider.
- Layer slider with tick markers on changed layers + flag glyphs; "jump to biggest
  change" button; status line shows current layer's Δt / Δe / flags.
- Dense-layer guard: merge collinear same-class segments before upload to GPU.

## Error handling

- File fails to parse / no extrusion moves → dialog, frame stays usable.
- <2 snapshots for session mode → picker shows file-browse hint.
- Layer-height mismatch → banner in canvas area ("layer heights differ — showing
  coincident layers only; see Settings/Feature tabs"), not an error.
- Snapshot evicted while selected → picker refresh, selection cleared gracefully.

## Testing

- Engine unit tests (Catch2, `tests/slic3r/` or `tests/libslic3r/` per fixture needs):
  - self-diff of a snapshot ⇒ 0 config rows, all layers identical, segments 100 % both;
  - synthetic mutation: delete support segments in a Z band ⇒ exactly those layers
    flagged `SUPPORT-CHANGED`, segments a_only;
  - unequal layer heights (0.20 vs 0.16 synthetic) ⇒ honest a_only/b_only, no crash;
  - proximity rescue: shift a segment 0.2 mm ⇒ jitter, 2 mm ⇒ real diff.
- Manual: re-slice same plate unchanged ⇒ overlay ~all gray/green (slicer determinism
  check); real preset A/B on one plate; two external Orca files.

## Increments

1. Snapshot capture + store + engine (no UI) + unit tests.
2. Frame with pickers, header, config/feature tables (no canvas).
3. Overlay canvas + slider + jump-to-change.
4. Side-by-side toggle + file mode + polish (menu items, banners, guards).
