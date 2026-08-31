# Support Interface Auto-Match (Part 2 v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in `support_interface_filament_source = nearest_surface`: support-interface extrusions are partitioned per extruder by nearest-wall vote against the 1–2 object layers above, so interfaces match the surface they touch.

**Architecture:** Reuse Part 1's engine (WallSampleIndex + vote/split/guard). A new pass in `Print::process` — after the object steps, BEFORE `psWipeTower` — partitions each support layer's interface entities into `SupportLayer::interface_by_extruder` (matched entities removed from `support_fills`; fallback-voted ones stay). ToolOrdering's existing per-support-layer block registers the assigned extruders at ctor time (data exists by then, so wipe-tower purge slots get planned). GCode emits partitions from a dedicated post-toolchange block in the per-extruder loop with per-instance origins — never through `ObjectByExtruder` buckets (they collide with the scalar base/interface buckets).

**Tech Stack:** C++17, libslic3r, Catch2 ([chameleon] tag), spike CLI harness.

**Spec:** `docs/superpowers/specs/2026-08-29-support-interface-match-design.md`

## Global Constraints

- Repo: worktree `C:\Dev\SnapmakerOrcaSupports`, branch `feat/color-matched-supports`. Never touch `C:\Dev\SnapmakerOrca`.
- Build/test commands + BUILD-SLOT RULE identical to Part 1's plan: check `Get-Process cl,link,MSBuild`; if busy, STOP with "WAITING FOR BUILD SLOT" (no polling/monitors); the controller signals. Full build via the Part-1 wrapper bat; tests `libslic3r_tests.exe "[chameleon]"`.
- Constants: match gap 2.0 mm; caps 3 switches/interface-layer, 20/object; sampling 0.8 mm; grid 2 mm; k=3; Part 1 tie-breaks. Fallback = the object's resolved interface extruder (0-based).
- Off-mode purity: `manual` (default) / single extruder / ByObject ⇒ pass never runs, `interface_by_extruder` empty, `support_fills` untouched, gcode byte-identical.
- Determinism: object ordinals (never raw ObjectID) in all keys/orderings; stable iteration everywhere.
- ToolOrdering base convention: `collect_extruders` pushes are 1-BASED (reindexed later by `reorder_extruders`); our map keys are 0-based → push `key + 1` there. GCode-side comparisons are 0-based.
- Lesson-enforced: partitioned entities MUST be removed from `support_fills` (a legacy print site emitting them under a scalar extruder is the Part 1 GUI bug); emission only after the toolchange append point.
- Commits: stage only named files.

## File Structure

| File | Responsibility |
|---|---|
| Modify `src/libslic3r/Layer.hpp` (~275 SupportLayer) | add `std::map<unsigned, ExtrusionEntityCollection> interface_by_extruder;` beside `support_fills` |
| Modify `src/libslic3r/BrimFilament.hpp/.cpp` | add `select_contact_layers()` (pure) + `partition_support_interfaces()` |
| Modify `src/libslic3r/PrintConfig.hpp/.cpp`, `src/libslic3r/Preset.cpp` | `SupportInterfaceFilamentSource` enum [manual|nearest_surface] + option beside `support_interface_filament` + print-options list entry |
| Modify `src/libslic3r/Print.cpp` | the assignment pass (new static fn + call after object steps ~L2560, before psWipeTower); invalidation entry pairing psWipeTower+psSkirtBrim-equivalent steps |
| Modify `src/libslic3r/GCode/ToolOrdering.cpp` (~714-721) | register assigned extruders per support layer (1-based push) |
| Modify `src/libslic3r/GCode.cpp` (per-extruder loop, after toolchange append — same site as the chameleon brim block) | dedicated interface-partition emission with per-instance origins |
| Modify `tests/libslic3r/test_chameleon_brim.cpp` | new unit tests |
| Modify `spike/verify_chameleon.sh` + new baseline | support off-mode identity + determinism checks (tshape fixture, tower on/off) |

---

### Task 1: Contact-layer selection + SupportLayer storage

**Files:** Modify `src/libslic3r/BrimFilament.hpp/.cpp`, `src/libslic3r/Layer.hpp`; Test `tests/libslic3r/test_chameleon_brim.cpp`.

**Interfaces (produces):**

```cpp
// BrimFilament.hpp
// Indices of object layers whose z-range overlaps (support_top_z, support_top_z + gap_mm].
// print_zs = ascending layer TOP z values; layer i spans (print_zs[i-1], print_zs[i]].
std::vector<size_t> select_contact_layers(const std::vector<double>& print_zs,
                                          double support_top_z, double gap_mm = 2.0);
```
`SupportLayer` (Layer.hpp:~283, next to `support_fills`): `std::map<unsigned, ExtrusionEntityCollection> interface_by_extruder; // chameleon: per-extruder matched interface partitions (empty = feature off)`.

- [ ] **Step 1: Failing tests**

```cpp
TEST_CASE("select_contact_layers picks the 1-2 layers above", "[chameleon]")
{
    std::vector<double> zs = {0.2, 0.4, 0.6, 0.8, 1.0};
    auto v = select_contact_layers(zs, 0.4, 2.0);          // support top at z=0.4
    REQUIRE(!v.empty());
    CHECK(v.front() == 2);                                  // first layer above (0.4,0.6]
    CHECK(v.back() <= 4);
    auto top = select_contact_layers(zs, 1.0, 2.0);         // nothing above the top
    CHECK(top.empty());
    auto vlh = select_contact_layers({0.2, 0.5, 1.4}, 0.2, 0.35); // VLH: only (0.2,0.55]
    REQUIRE(vlh.size() == 1);
    CHECK(vlh[0] == 1);
}
```
- [ ] **Step 2:** Build test target → FAIL. **Step 3:** Implement (binary search / linear scan over ascending zs: include index i when `print_zs[i] > support_top_z + EPSILON` and `layer bottom (i? print_zs[i-1]:0) < support_top_z + gap_mm`). Add the SupportLayer member.
- [ ] **Step 4:** Build + `[chameleon]` all PASS. **Step 5:** Commit:
```bash
git add src/libslic3r/BrimFilament.hpp src/libslic3r/BrimFilament.cpp src/libslic3r/Layer.hpp tests/libslic3r/test_chameleon_brim.cpp
git commit -m "feat(chameleon-p2): contact-layer selection + SupportLayer partition storage"
```

---

### Task 2: partition_support_interfaces + config key

**Files:** Modify `src/libslic3r/BrimFilament.hpp/.cpp`, `src/libslic3r/PrintConfig.hpp/.cpp`, `src/libslic3r/Preset.cpp`; Test file.

**Interfaces (produces):**

```cpp
// PrintConfig.hpp near BrimFilamentSource:
enum SupportInterfaceFilamentSource { sifsManual = 0, sifsNearestSurface };
// + CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SupportInterfaceFilamentSource)
// PrintConfig.cpp: map {"manual", sifsManual}, {"nearest_surface", sifsNearestSurface} + DEFINE;
// option def beside "support_interface_filament": label "Support interface filament source",
// tooltip "manual: use the configured support interface filament (default). nearest_surface:
// each interface region uses the filament of the model surface it touches.",
// comAdvanced, default sifsManual. Register in Preset.cpp print-options list beside
// "support_interface_filament". NOTE (Part 1 lesson): the option must live in a config class
// reachable as m_config/object config at the pass's call site - verify which class holds
// support_interface_filament (PrintObjectConfig) and mirror it; read it via object config.

// BrimFilament.hpp
// Partition the interface-role entities of `support_fills` by vote against `idx`.
// - Entities whose every vote == fallback stay in support_fills untouched (fast path).
// - Otherwise the entity is split; runs voted fallback are appended back into
//   support_fills as new interface paths; other runs go to out[extruder].
// - Non-interface entities are never touched. Matched originals are deleted.
// Returns switch-boundary count added (for the per-object cap accounting).
size_t partition_support_interfaces(ExtrusionEntityCollection& support_fills,
                                    unsigned fallback_extruder,
                                    const WallSampleIndex& idx,
                                    const BrimVoteParams& params,
                                    std::map<unsigned, ExtrusionEntityCollection>& out);
```

- [ ] **Step 1: Failing tests** (synthetic collections; reuse `segment()`; build support_fills with one erSupportMaterial path + two erSupportMaterialInterface paths):

```cpp
static ExtrusionEntityCollection make_support_fills(float y_iface)
{
    ExtrusionEntityCollection c;
    auto* base = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    base->polyline = Polyline({Point(scale_(0), scale_(-5)), Point(scale_(30), scale_(-5))});
    c.entities.push_back(base);
    for (int i = 0; i < 2; ++i) {
        auto* p = new ExtrusionPath(erSupportMaterialInterface, 1.0, 0.4f, 0.2f);
        p->polyline = Polyline({Point(scale_(0), scale_(y_iface + i)), Point(scale_(30), scale_(y_iface + i))});
        c.entities.push_back(p);
    }
    return c;
}

TEST_CASE("partition_support_interfaces splits by contact walls, base untouched", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);    // left wall above -> extruder 1
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);   // right wall above -> extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);
    REQUIRE(out.count(1) == 1);
    REQUIRE(out.count(2) == 1);
    // base path remains; matched interface originals removed
    size_t base_n = 0, iface_n = 0;
    for (auto* e : fills.entities) (e->role() == erSupportMaterial ? base_n : iface_n)++;
    CHECK(base_n == 1);
    CHECK(iface_n == 0);
}

TEST_CASE("partition_support_interfaces uniform-fallback fast path keeps entities", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 30, 6), 0, 1);    // only fallback-extruder walls
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    const size_t before = fills.entities.size();
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);
    CHECK(out.empty());
    CHECK(fills.entities.size() == before);          // untouched (off-parity)
}
```
- [ ] **Step 2:** FAIL. **Step 3:** Implement (walk entities; interface-role only; per-entity chain → votes → uniform-fallback fast path or `split_polyline_by_vote`; new `ExtrusionPath(erSupportMaterialInterface, ...)` per run copying flow attrs; fallback runs appended to support_fills; delete matched originals; rebuild entities vector; count boundaries). Add enum/option/Preset entries.
- [ ] **Step 4:** PASS + full app build exit 0. **Step 5:** Commit (list: BrimFilament.hpp/.cpp, PrintConfig.hpp/.cpp, Preset.cpp, test file; message `feat(chameleon-p2): interface partition engine + config key`).

---

### Task 3: Print::process pass + ToolOrdering registration

**Files:** Modify `src/libslic3r/Print.cpp`, `src/libslic3r/GCode/ToolOrdering.cpp`.

**Interfaces:** consumes Tasks 1-2 + Part 1's `chameleon_collect_wall_samples` (reuse for wall sampling with the object's per-region filament derivation) and ordinal pattern.

- [ ] **Step 1: The pass** (static fn in Print.cpp + call sited after the object-steps `tbb::parallel_for` (~L2560) and before `set_started(psWipeTower)` (~L2612); NOT inside psSkirtBrim):

Guard: any object has `support_interface_filament_source == sifsNearestSurface` (object config) AND `extruders().size() > 1` AND `print_sequence != ByObject`. Per such object (sequential, deterministic): build ordinal map (objects vector order); for each support layer SL of the object (skip if `SL.print_z <= first layer height + EPSILON` — plate guard):
  - `select_contact_layers(object_layer_print_zs, SL.print_z, 2.0)`; skip (fallback) if empty;
  - build a WallSampleIndex from those layers' `lr->perimeters` per region (reuse the Part 1 derivation incl. outer_wall_filament rule; instance shift NOT applied — supports and walls share object coordinates; object_key = ordinal);
  - fallback = the object's resolved interface extruder 0-based (`object.config().support_interface_filament.value > 0 ? v-1 : object default extruder`);
  - `partition_support_interfaces(SL.support_fills, fallback, idx, params, SL.interface_by_extruder)`;
  - accumulate switch count; past 20/object, stop partitioning further layers of that object (log one info line).
- [ ] **Step 2: ToolOrdering** (~L714-721, after the has_interface push): 
```cpp
        // Chameleon P2: register per-layer matched interface extruders (map keys are
        // 0-based; this collection phase is 1-based until reorder_extruders reindexes).
        for (const auto& kv : support_layer->interface_by_extruder)
            if (!kv.second.entities.empty())
                layer_tools.extruders.push_back(kv.first + 1);
        if (!support_layer->interface_by_extruder.empty())
            layer_tools.has_support = true;
```
- [ ] **Step 3:** Full build exit 0; `[chameleon]` tests still green. Off-mode CLI slice (tshape, manual mode) byte-identical to a freshly-recorded pre-change baseline (record it FIRST, before your edits, as `spike/out/p2_baseline.gcode`, tower OFF, using spike_support_overrides.json).
- [ ] **Step 4:** Commit (`feat(chameleon-p2): pre-ToolOrdering interface assignment pass + per-layer registration`).

---

### Task 4: GCode emission + integration + verify extension

**Files:** Modify `src/libslic3r/GCode.cpp`; `spike/verify_chameleon.sh`; new `spike/spike_p2_overrides.json`.

- [ ] **Step 1: Emission block** — same site as the chameleon brim block (immediately after `gcode += std::move(gcode_toolchange);`), addition:

```cpp
        // Chameleon P2: matched support-interface partitions for this extruder.
        for (const LayerToPrint& ltp : layers) {
            if (ltp.support_layer == nullptr) continue;
            const SupportLayer& sl = *ltp.support_layer;
            auto it = sl.interface_by_extruder.find(extruder_id);
            if (it == sl.interface_by_extruder.end() || it->second.entities.empty()) continue;
            const PrintObject& obj = *ltp.original_object;
            m_layer = ltp.support_layer;
            m_object_layer_over_raft = false;
            for (const PrintInstance& instance : obj.instances()) {
                this->set_origin(unscale(instance.shift));
                gcode += this->extrude_support(it->second, erSupportMaterialInterface);
            }
        }
```
(Verify local names — `layers`, `LayerToPrint::support_layer/original_object`, `set_origin(unscale(...))` shape — against the surrounding function; mirror how the existing instance loop sets origin/m_layer for supports. Iterate `layers` in its existing order — deterministic.)
- [ ] **Step 2:** Full build; `[chameleon]` green.
- [ ] **Step 3: Integration.** `spike/spike_p2_overrides.json` = spike_support_overrides.json + `"support_interface_filament_source": "nearest_surface"`. Slices (tshape, PLA+PETG): (a) manual mode byte-identical to `p2_baseline.gcode` (modulo the Part-1 normalization + new config line); (b) nearest_surface: exit 0, `; FEATURE: Support interface` present, M620 within [2, 2 + 2×3×layers-with-interface] (record actual; the single-material fixture may legally stay at 2 — the known CLI per-object-filament limitation applies; what MUST hold is (a) + no crash); (c) both (a) and (b) repeated with `"enable_prime_tower": "1"` — tower ON must also hold identity in manual mode and slice cleanly in nearest_surface.
- [ ] **Step 4:** Extend `verify_chameleon.sh` with the four support checks (manual-identity off/on tower, nearest_surface-sanity off/on tower) + determinism repeat; ALL PASS.
- [ ] **Step 5:** Commit (`feat(chameleon-p2): interface partition emission + tower on/off verification`). Report: ready for user GUI fixture (multi-material overhang plate) + physical validation.
