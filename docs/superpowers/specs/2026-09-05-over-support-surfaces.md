# Over-support surfaces — spec

Date: 2026-09-05 · Branch: `feat/over-support-surfaces` (from `feat/ultra-preferences` @ `e513088d61`)
Status: implemented, gated

## 1. What

Three new process settings, per object:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `over_support_surfaces` | bool | `0` (off) | Print the object's bottom faces that rest on support material like a bottom shell instead of like a bridge. |
| `over_support_flow` | float (flow ratio) | `1.0` | Flow ratio for those faces. Replaces **Bridge flow ratio** for them. |
| `over_support_speed` | float, mm/s | `0` | Speed for those faces. `0` = follow the layer's **outer wall speed**, so the face matches the perimeters framing it. Any other value is used as it is. Replaces **Bridge speed** for them. |

They live in the `Support` category and appear on the process tab's Support page, in their own
**Over-support surfaces** group between *Support ironing* and *Advanced*.

The affected faces get their own surface type (`stBottomOverSupport`), their own extrusion role
(`erBottomSurfaceOverSupport`) and their own preview feature type, **"Bottom surface over support"**,
drawn in turquoise `#29C2B8` (`{0.16, 0.76, 0.72}`) next to the existing indigo "Bottom surface".

## 2. Why

Today a bottom face lying on support with `support_top_z_distance > 0` is classified `stBottomBridge`
— the same class as a genuinely unsupported span over air. It therefore gets **Bridge flow ratio**,
**Bridge speed**, bridge density and **Thick bridges**. Those faces are usually among the most visible
surfaces of a print, and they are not bridges: they are lying on the support interface. The bridge
settings are tuned for spanning air, they are convoluted, and nothing in the UI tells the user that
changing "Bridge speed" will change how the underside of their part looks. The result is a face that
does not match the walls around it, and no obvious way to fix it without wrecking real bridges.

With the switch on, those faces are printed as what they are: a solid bottom shell, with the object's
normal bottom surface pattern and density, no thick bridges, no bridge angle detection, and a flow and
speed of their own. True bridges over air are untouched.

## 3. The classification rule, as built

`PrintObject::detect_surfaces_type` (`src/libslic3r/PrintObject.cpp`) decides, once per object:

```
over-support is possible  ⟺  over_support_surfaces
                          &&  has_support()                        (enable_support || enforce_support_layers > 0)
                          &&  support_top_z_distance > 0
```

The Z-gap condition is not a nicety: at a zero gap the fork already classifies these faces `stBottom`
(the soluble-interface rule), so there is nothing left to reclassify and this feature stands down.

Then, per support type — **the same predicate the fork already uses to decide "this bottom is fully
supported" in the soluble case, only at a non-zero gap**:

* `normal(auto)` — the generator supports every overhang it detects, **unless** `bridge_no_support` is
  on, in which case bridges deliberately get no support and stay bridges.
* `tree(auto)` — supported when `support_interface_top_layers > 0`, `max_bridge_length == 0` and
  `support_critical_regions_only` is off; otherwise the generator may leave the face unsupported and
  it stays a bridge.
* `normal(manual)` / `tree(manual)` — support exists only where the user asked for it, so only the
  part of the face covered by a **support enforcer** (enforcer volume or painted enforcer facets,
  projected onto that layer) counts as over support.

Finally, in every case, **support blockers are subtracted**: enforcer/blocker volumes and painted
facets are projected onto the object's layers by `PrintObject::slice_support_annotations`, which uses
exactly the recipe `SupportAnnotations` uses inside the support generator (same slices, same painted
facets, same blocker expansion). What is left over after the blockers stays `stBottomBridge` and keeps
the bridge settings.

The first layer never participates: it has no lower layer, is already `stBottom`, and is on the plate.

### 3.1 Why this is a reconstruction and not the generator's own contacts

The task asked for "the support generator's contact/roof areas for that layer". Those do not exist
when the decision has to be made. `Print::process()` runs `make_perimeters → infill → ironing →
generate_support_material`: supports are generated **after** slicing and after the fills exist, so
`detect_surfaces_type` (inside `posSlice`) cannot ask the generator anything. Classifying later would
mean regenerating the fills for the affected layers, which is a different and much larger change.

So the rule is a slice-time reconstruction. What it gets exactly right: whether support exists at all,
whether the support type will place support under detected overhangs, the `bridge_no_support` /
`max_bridge_length` / `support_critical_regions_only` refusals, and the user's enforcers and blockers.

What it cannot see, and where it can therefore over-claim:

* `support_on_build_plate_only` — a face above a *top* surface of the same object gets no support, but
  the classifier still calls it over-support. Computing `buildplate_covered` needs a downward
  propagation pass the classifier does not run.
* `support_remove_small_overhang`, the sharp-tail and cantilever heuristics, and the XY trimming that
  can leave a support column narrower than the face above it. A narrow fringe of a wide face can end
  up over air while being printed as over-support.
* The support generator's own minimum-area filters.

All of these are margins of a face that genuinely is on support, and the failure mode is cosmetic
(a fringe printed as a bottom shell rather than as a bridge), not structural. If a user hits one of
them, the escape hatch is a support blocker over the fringe, which the classifier does honour.

## 4. What stays a bridge

* Every bottom face over air on an object with no support.
* Every bottom face when `over_support_surfaces` is off — **byte-identical to today**, by
  construction: the classifier is gated on the key, `expand_merge_surfaces` for the new type returns
  before it touches anything when there are no such surfaces, and `discover_horizontal_shells` still
  runs exactly three surface-type iterations.
* `stInternalBridge` / `stSecondInternalBridge` — internal bridges over sparse infill. Untouched.
* Anything under `bridge_no_support` (normal auto), under a tree configuration that will not carry the
  face, or under a support blocker.
* Anything at `support_top_z_distance == 0`, which is already `stBottom` and already correct.

## 5. Where it acts

| Stage | File | What changed |
|---|---|---|
| Classification | `src/libslic3r/PrintObject.cpp` | `detect_surfaces_type` retypes the over-support part of each bottom bridge; `slice_support_annotations` projects enforcers/blockers; `discover_vertical_shells` / `discover_horizontal_shells` carry the new type; invalidation entries |
| Surface model | `src/libslic3r/Surface.hpp/.cpp` | `stBottomOverSupport` appended to `SurfaceType` (appended, so no existing value moves), `is_bottom()` and `is_bottom_over_support()`, debug SVG colour |
| External surfaces | `src/libslic3r/LayerRegion.cpp` | its own `expand_merge_surfaces` pass with the bottom-shell expansion parameters |
| Fill | `src/libslic3r/Fill/Fill.cpp` | role `erBottomSurfaceOverSupport`; pattern/density/flow already come from the bottom-surface branch because `is_bottom()`, `is_external()` and `is_solid()` all hold and `is_bridge()` does not; ironing "iron everything" includes it |
| Role | `src/libslic3r/ExtrusionEntity.hpp/.cpp` | `erBottomSurfaceOverSupport` inserted after `erBottomSurface`; name `"Bottom surface over support"` both ways (this is the `;TYPE:` token in the G-code and the preview legend label) |
| G-code | `src/libslic3r/GCode.cpp` | flow `*= over_support_flow`; speed from `over_support_speed`, falling back to `outer_wall_speed`; `extrusion_role_to_string_for_parser` |
| Preview | `src/slic3r/GUI/GCodeViewer.cpp` | colour table entry and calibration-thumbnail visibility |
| UI | `src/slic3r/GUI/Tab.cpp`, `ConfigManipulation.cpp` | the Support-page group; the flow and speed fields grey out unless the switch is on and the switch greys out unless supports are on with a non-zero Z gap |
| Keys | `src/libslic3r/PrintConfig.*`, `Preset.cpp` | the three defs, `part_support_keys()`, the process preset's known-key list |

`PrintConfig.cpp`'s `handle_legacy` is untouched: these are new keys, and a project that does not
carry them simply gets the defaults (switch off = today's behaviour).

`Slicing.cpp`'s `soluble_interface` path is untouched too, and deliberately: `soluble_interface` is
`support_top_z_distance == 0`, which is precisely the case this feature refuses to enter.

## 6. Support sets and groups

The three keys are in the `Support` category and are `PrintObjectConfig` members, so they join
`support_set_keys()` automatically (that list is derived, not hand-written — see
`src/libslic3r/SupportSet.cpp`), and they were added by hand to the curated `part_support_keys()`.

They are therefore **set-eligible and part-eligible today**: a saved support set carries them, and a
part may carry them in its own config and they survive the 3MF round trip and the group resolver.
They are **not per-part in behaviour**: like `support_top_z_distance`, `support_style` and
`support_threshold_angle` (tier B of the support-sets plan §3.5), the object's own value is what
`detect_surfaces_type` reads. Stage 5 of `docs/superpowers/plans/2026-09-02-support-sets-and-groups.md`
is where the per-part path gets wired; until then a per-part value is stored, resolved and displayed
but the object-wide value acts.

## 7. Gate

* `tests/fff_print/test_over_support_surfaces.cpp` (`[OverSupport]`): off-mode role identity (no new
  role, no new surface type, bridges still bridges), on-mode role presence, feedrate == the outer wall
  speed at `over_support_speed = 0` and == the set value otherwise, extrusion-per-millimetre scaling
  with `over_support_flow`, and the four stand-down cases (`bridge_no_support`, supports off, zero Z
  gap, defaults).
* `scripts/support_group_identity.py` with the `over_support_off` corpus case: a support configuration
  that reads exactly the inputs this feature reads (`support_top_z_distance = 0.2`, interface layers),
  with the switch left at its default, compared against a baseline build that does not have the
  feature at all. It must stay within the tolerance gate along with every other off-mode case.

### 7.1 One deviation the gate needed

The support-sets plan §3.7 could say "the only new config key never reaches `full_print_config`",
because `support_group` is a `ModelVolume` key. These three are `PrintObjectConfig` members, so they
**do** reach `full_print_config` and therefore the G-code `CONFIG_BLOCK`. `diff_configs`
(`src/libslic3r/SliceCompare/Diff.cpp`) counts a key present on one side only as a changed row, so a
baseline that has never heard of `over_support_flow` would fail the "zero changed config rows"
criterion on **every** case, for the mere existence of a new setting.

`scripts/support_group_identity.py` therefore gained `effective_config_rows()` and a small
`NEW_CANDIDATE_KEYS` list: a row is forgiven only when the key is on that list **and the baseline
value is missing entirely**. A new key whose value differs between two builds that both have it is
still a changed row, and any other one-sided key is still a changed row. This is the same kind of
narrow, named normalisation the script already applies to the `; generated by` header and the
`id:<N>` token, and it is the only thing that had to move for this feature.
