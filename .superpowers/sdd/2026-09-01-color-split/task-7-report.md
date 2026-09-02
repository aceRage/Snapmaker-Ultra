# Task 7 report — Model mutation and coordinate space

Commit: `9f5e44dcfe feat(color-split): apply split to the model in place with rotation-safe placement`
Branch: `feat/color-split` in `C:\Dev\SnapmakerOrcaNext`

## What was implemented

### `src/libslic3r/ColorSplit.hpp`
- Forward declaration of `ModelVolume` (header still free of `Model.hpp` and Manifold).
- `struct ColorSplitSpace { Transform3d to_split, from_split; double depth_scale; bool world_path; }`.
- `ColorSplitSpace color_split_space(const ModelObject &, const ModelVolume &)`.
- `ColorSplitDepths scale_depths(const ColorSplitDepths &, double s)`.
- `std::vector<ModelVolume *> apply_color_split(ModelObject &, size_t source_volume_idx, ColorSplitResult &&, const ColorSplitSpace &, bool solid_interfaces, bool keep_base_sparse_infill)`.

### `src/libslic3r/ColorSplit.cpp` (+ `#include "Model.hpp"`, `<utility>`)
- **`color_split_space`** (spec 3.9). `T = instance[0] matrix × volume matrix` (identity instance matrix if the
  object has no instance). Isotropy test: the three column norms of `T.linear()` agree to `1e-6` relative — a
  rotation × uniform scale × any mirror leaves them equal, so rotation and mirroring stay on the cheap
  mesh-space path and only real anisotropy is rejected. Isotropic → `depth_scale = s`, identity transforms;
  anisotropic → `world_path = true`, `to_split = T`, `from_split = T⁻¹`.
- **`scale_depths`** — divides `D`, `ws`, `cap_top`, `cap_bottom`, `layer_height` by `s` (`unlimited` and an
  infinite `D` pass through unchanged).
- **`add_split_volume`** (file-static). Builds the `TriangleMesh`, and **on the world path calls
  `mesh.transform(space.from_split, /*fix_left_handed=*/true)`** — I picked the `TriangleMesh::transform`
  variant over the brief's `its_transform` + manual `flip_triangles()`: it is the same operation
  (TriangleMesh.cpp:337-347 flips when `det < 0`), it keeps `m_stats.volume` consistent, and it is exactly
  what spec §3.9 names for both legs of the round trip. Then `object.add_volume(src, std::move(mesh))` (the
  only public way in; the `(other, mesh&&)` constructor is private), followed by the rotation/scale-safe
  offset correction `src.get_offset() + src.get_transformation().get_matrix_no_offset() * v->mesh().get_init_shift()`,
  the name, and the resets: `text_configuration`, `emboss_shape`, `cut_info = ModelVolume::CutInfo()`
  (`invalidate_cut_info()` only clears `is_connector`, so it does **not** fully reset — verified at
  Model.hpp:846-858), `source = ModelVolume::Source()`, `set_type(MODEL_PART)`. Facet annotations are empty by
  construction (asserted in the private constructor, Model.hpp:1162-1164), so no stale `used_states` reach
  `PrintApply`.
- **`apply_color_split`** (spec §4). Body first (skipped when `r.body.indices.empty()`), then the pieces
  ascending; colour parts get `config.set("extruder", filament)` and, with `keep_base_sparse_infill`,
  `sparse_infill_filament = max(1, body_extruder)`. The body gets an explicit `extruder` only when the source
  had a **non-zero** one (an explicit 0 means "inherit" to `ModelVolume::extruder_id` just as a missing key
  does), otherwise the key is erased so the Ultra `outer_wall_filament` reset is not re-triggered. The new
  volumes are `std::rotate`d from the back of `object.volumes` into the source's slot and the source is
  deleted by index; `solid_interfaces` sets the object's `interface_shells`; `invalidate_bounding_box()`.
  An empty result (no body, no pieces) returns early and leaves the object completely untouched.

## Deviations from the brief (all minimal, all justified)

1. **`object.config.opt_bool("interface_shells")` → `object.config.get().opt_bool(...)`.** `ModelConfig`
   (PrintConfig.hpp:2034-2109) exposes `opt_int`, `opt_float`, `extruder()`, `has`, `set`, `erase` but **no
   `opt_bool`**; the brief's line does not compile. `.get()` returns the `DynamicPrintConfig`, whose
   `opt_bool` is Config.hpp:2487. Assertion meaning unchanged.
2. **`painted_model` names the volume.** `ModelObject::add_volume(const TriangleMesh &)` builds the volume
   through `ModelVolume(object, mesh)` (Model.hpp:1090), which leaves `name` empty — `object->name` is the
   *object's*. The brief's `REQUIRE(object.volumes[0]->name == "split-test")` is about the **body keeping the
   source volume's name**, so the helper now sets `volume->name = "split-test"` and the assertion tests real
   behaviour. Added `REQUIRE(object.volumes[1]->name == "split-test F2")` to pin the part naming too.
3. **World-path test fixture: `make_grid_box(40,40,20,6,6)` + whole top + `no_cap_no_step()`,** not
   `make_cube` + `ColorSplitParams{}`. Two independent reasons the brief's `pb.size().z() ≈ 1.5` could not
   hold on that fixture:
   - `depths_for_test(1.5)` sets `cap_top = 0.8`, and with the default `flat_cap = true` the painted cube top
     is a flat group with a core wider than 3 wall stacks, so it is **capped at 0.8**, not built at D = 1.5
     (ColorSplitShell.cpp:145-170).
   - Even with the cap off, a plain `make_cube`'s top face has only the four **corner** vertices, whose
     angle-weighted normals are the (±1,±1,1)/√3 bisectors — the drop is 1.5/√3 = 0.866 mm, as the existing
     test at test_color_split.cpp:283-305 documents at length.
   A grid box's top has interior vertices with exactly vertical normals, so the piece really is 1.5 mm deep
   in z. The test also now asserts `pb.size().x() ≈ 40` (the world split ran at x2; only a correctly applied
   `from_split` brings it back to 40), which is a sharper check of the round trip than the brief had.
4. **`apply_color_split` returns early on an empty result** (one line + a covering test). Without it, a
   result with no body *and* no pieces would delete the source and leave the object with zero volumes. Not
   reachable through `split_volume_by_paint` today (body + pieces always tile the input), but the function is
   public and the failure mode is model corruption.

## Tests (all in `tests/libslic3r/test_color_split.cpp`, tag `[colorsplit]`)

Four from the brief plus four covering the self-review edge cases:

| Test | Covers |
|---|---|
| apply replaces the source by body + parts in place with extruders set | names, extruders, MODEL_PART, no paint left, `interface_shells`, fresh unique volume/config ObjectIDs, world bbox preserved |
| a rotated, scaled and mirrored PART stays in place | isotropy detection with mirror, `depth_scale == 1.5`, the offset correction |
| anisotropic instance scale takes the world path | `world_path`, depth in world mm (1.5 in z), `from_split` undoing the x2 (40 mm in x), bbox preserved |
| the world path brings a mirrored part back outward-facing (new) | `det(T) < 0` round trip: both created volumes keep positive `its_volume` |
| empty body removes the source and keeps only pieces | empty body ⇒ the first piece takes the slot |
| the new volumes take the source's slot, between the modifiers around it (new) | **non-zero `src_idx`**, modifiers before and after keep their order, body first, `solid_interfaces = false` touches nothing |
| a source with no extruder key of its own... (new) | body's `extruder` erased and inherited (== 3), part's `extruder` == 2, `keep_base_sparse_infill` ⇒ `sparse_infill_filament == 3` |
| an empty result leaves the object untouched (new) | the early-return guard |

### TDD evidence

**RED** — tests written first, build before any implementation existed (68 `error C` lines saved to the
scratchpad `red_build.txt`), e.g.:

```
test_color_split.cpp(1280,5): error C2065: 'ColorSplitSpace': undeclared identifier
test_color_split.cpp(1280,29): error C3861: 'color_split_space': identifier not found
test_color_split.cpp(1289,20): error C3861: 'apply_color_split': identifier not found
test_color_split.cpp(1326,48): error C3861: 'scale_depths': identifier not found
```

**GREEN** — after implementing (final build, clean, no warnings from the changed files):

```
[colorsplit]        All tests passed (533 assertions in 45 test cases)
[colorsplit_spike]  All tests passed (10 assertions in 2 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.hpp` (+25)
- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` (+105)
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` (+225)

## Self-review findings (fixed before committing)

- **All tests used `src_idx == 0`**, so a wrong `std::rotate` first argument (e.g. `begin() + 1`) would have
  gone unnoticed. Rewrote the slot test to put a modifier **before** and **after** the source and call
  `apply_color_split(object, 1, ...)`. Passes.
- Verified against the real declarations rather than the brief's guesses: `ModelConfig` has no `opt_bool`;
  `invalidate_cut_info()` does **not** fully reset `CutInfo` (only `is_connector`), hence the explicit
  `cut_info = ModelVolume::CutInfo()`; `add_volume(const TriangleMesh &)` leaves the name empty.
- `ModelVolume &src` stays valid across the `add_volume` calls (`ModelVolumePtrs` is a vector of pointers, so
  reallocation does not move the volume itself); `created` holds pointers, so `std::rotate` and
  `delete_volume` do not invalidate it.

## Concerns / notes for the controller

1. **`delete_volume` collapses the transform when one volume is left.** `ModelObject::delete_volume`
   (Model.cpp:1407-1432) folds the last remaining volume's matrix into every instance when the object drops to
   a single volume. That is pre-existing Model behaviour and preserves world placement, and the empty-body
   test hits it, but the GUI job (Task 8+) should be aware that a single-piece split re-assigns a new
   `ObjectID` to that volume through this path.
2. **`color_split_space` uses the first instance only,** per spec 3.9. Other instances with a *different*
   anisotropic scale get an approximate depth — the documented limit; nothing in this task warns about it. If
   the dialog should say so, that belongs to the GUI task.
3. **`scale_depths` does not guard `s <= 0`.** A zero column norm falls to the world path (where `T⁻¹` would
   also be singular). Not reachable through the GUI, so no guard was added.
4. `apply_color_split` does not bounds-check `source_volume_idx`, matching `ModelObject::delete_volume(size_t)`.

---

# Fix round 1 — review findings

Commit: `fix(color-split): scale the depth override, keep paint in mesh space (Ruling 23)`
All of `[colorsplit]` (588 assertions / 53 cases), `[colorsplit_spike]` (10 / 2) and `[paintdepth]` (1568 / 94) green.

## Important — `depth_override_mm` is a world length

`ColorSplitDetail::effective_depths` applies `params.depth_override_mm` **inside** the split, after the caller
has already scaled the depths, so on the mesh-space path an unscaled override cut at its world value.

- Added `ColorSplitParams scale_params(const ColorSplitParams &, double s)` next to `scale_depths` — it divides
  `depth_override_mm` by `s` when positive and leaves zero/negative (= "no override") alone. Nothing else in
  `ColorSplitParams` carries a length.
- `ColorSplit.hpp` now states that on the mesh-space path **both** `scale_depths` and `scale_params` must be
  applied before calling `split_volume_by_paint` (and that neither is on the world path), and that
  `ColorSplitResult` — meshes **and** `depths` — comes back in split space (multiply by `depth_scale` to
  display).
- New test *"the depth override is a WORLD length on the mesh-space path"*: a volume at isotropic 2x with
  `depth_override_mm = 1.0` reports `r.depths.D == 0.5` (split space), `r.depths.D * depth_scale == 1.0`, and
  the applied piece measures 0.5 mm in mesh z and 1.0 mm in world z.

## Ruling 23 — paint is always resolved in mesh space

`split_volume_by_paint` gained a trailing `const Transform3d &to_split = Transform3d::Identity()`. The paint is
extracted from the untransformed mesh, and only then is `patches.surface` carried across with
`its_transform(surface, to_split, /*fix_left_handed=*/true)`. The left-handed fix swaps two vertices of every
facet; on the raw mesh that would re-order the vertices the paint's sub-facet subdivision is expressed against
and mirror the stroke inside every partially painted facet, whereas on the retriangulated surface it only
reverses each facet's own winding — facet order, and so `facet_state`, is untouched (`its_flip_triangles`-style
in-face swap, TriangleMesh.cpp:736-740). The identity default keeps everything in mesh space, and the transform
is skipped entirely when `to_split` is identity.

- Both world-path tests now pass `space.to_split` instead of pre-transforming the mesh.
- New test *"a partially painted facet is not mirrored by the world path"*: a `TriangleSelector::Sphere`
  cursor (radius 6, top centre) stroke is split (a) untransformed and (b) on a mirrored volume inside an
  instance scaled (2,1,1) — `det(T) < 0`, world path. The world piece mapped home with `from_split` has
  positive volume and matches the reference within 1e-3 relative. This holds exactly because the depth runs
  along z, which neither the mirror nor the x2 touches, so `|det(T⁻¹)| = 0.5` cancels the doubled patch area.
- **The test is demonstrably non-vacuous.** It also runs the *old* contract (transform the raw mesh with the
  left-handed fix, then read the paint) and requires the result to differ. MEASURED: the old contract does not
  merely shift the stroke — it produces **zero pieces** on this fixture. The assertion is written as "does not
  reproduce the reference volume" rather than pinning that count, which is an artefact of the broken path; an
  `INFO` records the piece count and volume for future diagnosis.

## Minors

- **Isotropy test hardened.** Equal column norms alone accept a rotated anisotropic scale (`R S Rᵀ`), and
  `Geometry::Transformation` carries such matrices — `ModelObject::delete_volume` builds one by multiplying an
  instance matrix by a volume matrix. `color_split_space` now also requires the three off-diagonal entries of
  `LᵀL` to be below `1e-6·s²`. New test builds `R S Rᵀ` with `S = diag(2,1,1)` and R a 45° turn about z,
  asserts the first two column norms are equal (the trap) and that the world path is still taken.
- **Degenerate transforms refused.** `color_split_space` throws `ColorSplitError("Degenerate part
  transformation.")` when the smallest column norm is ≤ 1e-9 or `|det(L)| ≤ 1e-9·sx·sy·sz`, instead of
  inverting a singular matrix. New test squashes the volume with `diag(1,1,0)` and expects the throw.
- **`apply_color_split`**: empty pieces are now skipped with the same guard as the body (they are dropped
  upstream with a warning, and an empty volume would only clutter the object list), and
  `assert(src_idx < object.volumes.size() && object.volumes[src_idx]->is_model_part())` documents the
  precondition.
- **New tests**: `text_configuration` / `emboss_shape` reset and `cut_info` back to default on the created
  volumes (all three set on the source first, including a real `CutInfo(Plug, 0.5f, 0.5f, false)` — the test
  checks `is_connector == false`, `is_processed == true`, `is_from_upper == true`, which
  `invalidate_cut_info()` alone would not give); a source with an explicit `extruder = 0` (the body must have
  **no** extruder key, since an explicit zero means "inherit" to `ModelVolume::extruder_id`); an object with
  two instances (the second offset and rotated — both keep their world bounding box); and a rotated volume on
  the world path (anisotropic instance + volume rotation and offset) staying in place.
- **Multi-instance approximation note** moved into the `ColorSplitSpace` header comment.

## Files changed in this round

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.hpp`
- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp`
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp`

## Concerns

- The Ruling 23 equality holds *exactly* on this fixture because the split depth is along z and the anisotropy
  is in x. A fixture whose anisotropy shared an axis with the depth would legitimately produce a different
  piece, so this test pins the paint-orientation contract, not a general invariance claim. That is the claim
  Ruling 23 makes, but it is worth stating.
- `assert` compiles out in the Release test build, so the precondition is documentation there rather than an
  enforced check. The GUI job (Task 8+) still owns validating the index and the MODEL_PART gate.
