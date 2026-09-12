# Housekeeping, 2026-09-12

Branch `fix/housekeeping-0912`, off `feat/ultra-preferences` (`944457b8bb`). Four items
from the owner's brief. One commit per item.

---

## Item 1 — Bed texture rendering

### The premise was wrong: bed textures already render

The brief (following `2026-09-12-creality-anycubic-profiles.md`) reports that
`m_texture` / `render_texture()` in `src/slic3r/GUI/3DBed.cpp` are commented out, and
concludes the profiles' `bed_texture` SVG/PNG "never draw". The first half is true; the
conclusion is not.

**`bed_texture` reaches the screen through `PartPlate`, not through `Bed3D`.** This fork
inherits Bambu Studio's architecture, in which the *part plate* owns the bed surface and
Bed3D draws only the model STL, the grid and the axes. The live path is:

| Step | Location |
|---|---|
| Read `bed_texture` from the selected printer preset | `Plater.cpp:24216` `Plater::set_bed_shape()`, via `PresetUtils::system_printer_bed_texture` (`Preset.cpp:4018`); `bed_custom_texture` overrides it |
| Hand it to the plate list | `Plater.cpp:17668` -> `PartPlateList::set_shapes(..., custom_texture, ...)` |
| Validate + store (the live twin of the commented-out `check_texture`) | `PartPlate.cpp:5248` `PartPlateList::update_logo_texture_filename()` |
| Rasterise SVG / load PNG, upload to GPU | `PartPlate.cpp:738` `PartPlate::render_logo()` |
| Draw with the GL3 `printbed` shader | `PartPlate.cpp:692` `PartPlate::render_logo_texture()` |

`Bed3D::render_texture()` is dead code, but it is *redundant* dead code: it is the
pre-plate Bambu implementation, superseded by `PartPlate::render_logo`. Both drew the same
texture with the same `printbed` shader onto the same quad. Reviving it as the brief
describes would have drawn every bed texture **twice**, co-planar and blended, at
`GROUND_Z` (Bed3D) versus `GROUND_Z + 0.02` (PartPlate) — z-fighting plus doubled alpha.

The commented-out code is also stale in a way that proves it has not been live in this
fork: it binds the vertex attribute `v_tex_coords` (`3DBed.cpp:561`), but both
`printbed.vs` shaders declare `v_tex_coord`, singular
(`resources/shaders/{110,140}/printbed.vs`), which is what the live `GLModel` path looks up
(`GLModel.cpp:705`). Restored verbatim it would have bound nothing and rendered an
untextured quad.

`git log -S` confirms the lines were never live here: they arrive already commented out in
`1555904bef` ("Add the full source of BambuStudio"), the initial import. The same lines are
commented out in the reference clone at `C:\Dev\BambuStudio` (`3DBed.cpp:472`). Upstream did
not "keep it working" — upstream disabled it for the same reason, when PartPlate took over.

### What the vendor gate actually does

`Tab::on_presets_changed()` (`Tab.cpp:2101`) sets the plate render mode from the vendor:

```cpp
bool is_bbl_vendor_preset = wxGetApp().preset_bundle->is_bbl_vendor();
if (is_bbl_vendor_preset) {
    wxGetApp().plater()->get_partplate_list().set_render_option(true, true);   // bed-type logos
} else {
    wxGetApp().plater()->get_partplate_list().set_render_option(false, true);  // profile bed_texture
}
```

`render_bedtype_logo` selects between two texture sources in `render_logo()`: Bambu printers
draw the built-in per-bed-type plate art (Cool Plate, Engineering Plate, ...), everything
else draws the profile's own `bed_texture`. Both branches work; this is correct, not a bug.

So Snapmaker U1, Creality, FlashForge and Anycubic (PNG and SVG alike) all render their
`bed_texture` today, and Bambu renders its bed-type art. Verified by reading the profile
data through to the files on disk — `Creality K2 Plus` -> `creality_k2plus_buildplate_texture.png`,
`Creality K2 Pro` -> `creality_k2pro_buildplate_texture.svg`, `Bambu Lab A1` -> `bbl-3dp-logo.svg`
— each present on disk and reachable by the path above.

### The Snapmaker U1's blank bed is deliberate, not a bug

The brief lists the U1 as "empty SVG now", implying breakage. It is an intentional change
the owner made the day before, in `a41f0287c8` (2026-09-11):

> Snapmaker U1 bed texture: drop the vendor wordmark (the texture was only the nine letter paths)

`resources/profiles/Snapmaker/Snapmaker U1_texture.svg` is a valid 349-byte SVG with a
correct `viewBox` and an empty `<g>`, carrying a comment saying the wordmark was removed on
purpose and that the plate outline comes from the bed model. It rasterises fine and draws a
fully transparent quad. **Not reverted** — reinstating the wordmark would undo a deliberate
decision, and the brief gives no authority to.

### Degradation on a missing or unreadable texture

Already correct, and checked on every branch:

- `update_logo_texture_filename()` (`PartPlate.cpp:5248`) tests extension and existence with
  a `boost::system::error_code` overload, so a permission error returns false instead of
  throwing; a bad path logs at `error` and clears the filename.
- An empty filename resets the texture and returns before any GL call (`PartPlate.cpp:742`).
- A rasterisation or PNG-decode failure logs at `warning` and returns, leaving the plate
  untextured (`PartPlate.cpp:768`, `:784`).
- An unsupported extension logs at `warning` and returns (`:789`).
- The draw is additionally guarded by `m_logo_triangles.is_initialized()` (`:804`).

No path is fatal. Nothing to fix.

### Decision

**No code change.** The feature works; the dead code is superseded, not lost; the one
apparent regression is an intentional commit from the day before. Deleting the commented-out
`Bed3D` block was considered and rejected for this branch: it is inert, it documents the
pre-plate design, and removing ~120 lines of commented code in `3DBed.cpp` risks colliding
with the Edit-gizmo and GLGizmoCut agents working nearby. The PC render path is therefore
trivially bit-identical, textured or not.

This item is documentation plus the click-test below, which is what the brief asks for in
the "otherwise" case: the rasterisation is *not* factored into libslic3r (it lives in
`GLTexture::load_from_svg_file`, GUI-side, and needs a GL context), so no unit-level test is
possible and the click-test is the verification.

### Click-test

Run the slicer and, on the Prepare tab with the plate visible from above:

1. **Third-party PNG.** Select printer *Creality K2 Plus*. The plate shows the Creality
   build-plate artwork (PNG). Confirms the non-SVG branch and `svg_source=0` in the shader.
2. **Third-party SVG.** Switch to *Creality K2 Pro*. Artwork changes to the SVG plate.
   Confirms nanosvg rasterisation and the radial-gradient `svg_source=1` branch.
3. **Bambu bed-type art.** Switch to *Bambu Lab A1*. The plate shows Bambu's bed-type
   artwork, and changing Bed Type in the sidebar (Cool / Engineering / High Temp) swaps it.
   Confirms `render_bedtype_logo=true` and that the vendor gate flips.
4. **No texture.** Switch to *Snapmaker U1*. The plate is clean — grid, bed model, no
   wordmark. This is the expected post-`a41f0287c8` look, not a failure.
5. **Missing file (degradation).** Temporarily rename
   `resources/profiles/Creality/creality_k2plus_buildplate_texture.png`, restart, select
   K2 Plus. Expect an untextured plate, no crash, and one `Unable to load bed texture:`
   line in the log. Restore the file.
6. **Dark/light.** Toggle the app theme with a textured plate selected, then again with the
   U1. The texture is unaffected by theme (it is not tinted); the bed model and grid recolour
   via `DEFAULT_MODEL_COLOR_DARK`. Check the plate stays legible in both.
7. **Bottom view.** Orbit below the plate on a textured printer. The texture renders
   back-facing with `transparent_background=1` (front face flipped to `GL_CW`), i.e. visible
   but translucent — not black, and not z-fighting.

Step 7 is the one that would have caught the double-draw had `Bed3D::render_texture` been
revived as briefed.
