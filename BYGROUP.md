# ByGroup Print Sequence

## Goal

A hybrid print sequence mode where objects are assigned to numbered groups. Within a group, objects are printed by layer (interleaved, orientation preserved). Groups are printed sequentially — one group finishes completely before the next begins. Individual objects can still be printed by object (group of 1).

This sits between the existing `ByLayer` (all objects interleaved) and `ByObject` (each instance fully before next) modes.

## Relevant Code

### Config / Enum

- `src/libslic3r/PrintConfig.hpp:148` — `PrintSequence` enum (`ByLayer`, `ByObject`). Add `ByGroup` here.
- `src/libslic3r/PrintConfig.cpp:295` — string-to-enum map. Add `"by group"` entry.
- `src/libslic3r/PrintConfig.cpp:1829` — `print_sequence` setting definition.
- Need a new per-object integer config `print_group` (like the existing `extruder` per-object config) so each object can be assigned a group number. Group 0 or unset = ByObject behavior (group of 1).

### Print Validation

- `src/libslic3r/Print.cpp:1284` — ByObject mode enforces collision clearance between instances. ByGroup needs the same check, but only between groups (objects in the same group are allowed to be close; objects in different groups need clearance since they will be printed at different times).

### GCode Generation — Three Key Sites

**Tool ordering setup (`GCode.cpp:2886`)**
- ByObject builds per-instance `ToolOrdering`.
- ByLayer builds one global `ToolOrdering`.
- ByGroup needs one `ToolOrdering` per group (same as ByLayer but scoped to the group's objects).

**Layer count for progress bar (`GCode.cpp:2647`)**
- ByObject: counts layers per instance × instance count.
- ByLayer: counts unique Z values across all objects.
- ByGroup: sum of unique Z values per group (each group contributes its own layer set).

**Main print loop (`GCode.cpp:3425`)**
- ByObject: outer loop = instances, inner loop = layers of that instance.
- ByLayer: outer loop = layers (all objects at each Z).
- ByGroup: outer loop = groups (sorted by group number), inner loop = layers (ByLayer logic filtered to that group's objects). Between groups, emit the same Z-lift/travel/origin-reset that ByObject does between instances (`GCode.cpp:3453`).

**Per-layer instance ordering (`GCode.cpp:5239`)**
- Currently branches on `ByObject` vs everything else.
- ByGroup within a layer behaves like ByLayer — no change needed here if the outer loop already filters to the active group.

## Implementation Plan

- [x] **1. Add `ByGroup` to the enum and config**
  - `PrintConfig.hpp:148` — add `ByGroup` to `PrintSequence`
  - `PrintConfig.cpp:295` — add `{ "by group", int(PrintSequence::ByGroup) }`
  - `PrintConfig.cpp:1829` — update enum definition comment/docs
  - Add `print_group` as a per-object `coInt` config (default 0 = treated as its own group / ByObject behavior). Model this after how `extruder` is defined as a per-object override.

- [x] **2. Group construction helper**
  - Add a helper (probably in `Print.cpp` or a new utility) that, given a `Print`, returns `std::vector<std::vector<const PrintInstance*>>` — one inner vector per group, sorted by group number. Instances with `print_group == 0` each get their own singleton group (ByObject).

- [x] **3. Collision validation (`Print.cpp:1284`)**
  - For `ByGroup`: run the same extents/clearance check as ByObject, but treat each group as a single unit. Objects within the same group do not need clearance from each other.

- [x] **4. Tool ordering setup (`GCode.cpp:2886`)**
  - For `ByGroup`: iterate groups; build `ToolOrdering` per group from that group's objects (same as ByLayer does globally, but scoped). Store as a vector indexed by group.
  - Added new `ToolOrdering(vector<const PrintObject*>, ...)` constructor and matching `sort_and_build_data` overload in `ToolOrdering.hpp/.cpp` that collect Z levels and extruders from all group objects.

- [x] **5. Layer count (`GCode.cpp:2647`)**
  - For `ByGroup`: for each group, collect unique Z values across that group's objects and sum them.

- [x] **6. Main print loop (`GCode.cpp:3425`)**
  - Added `ByGroup` branch at `GCode.cpp:3779`:
    - Outer loop: iterate `print_groups` in order
    - Per group: build `group_ordering` (filtered from global instance ordering), build `group_objects`, rebuild `ToolOrdering` from all group objects
    - Between groups: Z-lift + rotation gcode + travel to origin (mirrors ByObject inter-instance transition)
    - Per group: `collect_layers_to_print_for_group` → `process_layers` with group-scoped layers and ordering
  - `collect_layers_to_print_for_group` (`GCode.cpp:2084`): free static function that returns the same `vector<pair<coordf_t, vector<LayerToPrint>>>` format as `collect_layers_to_print(const Print&)`, but with only group objects populated; non-group object slots are null `LayerToPrint()`.

- [x] **7. GUI — per-object table (`GUI_ObjectTable.cpp/.hpp`)**
  - Added `col_print_group` / `col_print_group_reset` at enum positions 6–7 (right after extruder columns).
  - Added `print_group` column in `init_cols` at the matching push_back position.
  - Added `print_group` loading in `construct_object_configs`.
  - Added `"print_group"` to `s_Preset_print_options` in `Preset.cpp` (both locations) so `global_config.option<ConfigOptionInt>("print_group")` returns non-null.

- [x] **8. GUI — per-object settings panel (`TabPrintModel`)**
  - `PrintConfig.cpp`: gave `print_group` `def->category = L("Others")`.
  - `Tab.cpp:2737`: appended `print_group` option line to the **Others → Special mode** optgroup (next to `print_sequence` / `print_order`).
  - Mechanism: `TabPrintModel::build()` reuses `TabPrint::build()` pages then keeps only keys in `PrintObjectConfig().keys() ∩ Preset::print_options()`. `print_group` satisfies both (it's in `PrintObjectConfig` at `PrintConfig.hpp:1078` and in `s_Preset_print_options`), so it survives the filter and appears in the right-side per-object panel.

- [x] **9. 3mf persistence ("embed print_group like layer orientations")**
  - No new code needed. `layer_rotation_b/c` have **no** special 3mf serialization — they persist purely through the generic per-object `ModelConfig` path. `print_group` is a `PrintObjectConfig` key, so once set as a per-object override it is written by `_3MF_Exporter::_add_model_config_file_to_archive` (iterates `obj->config.keys()`, `3mf.cpp:3107`) and read back by the generic `model_object->config.set_deserialize(...)` on import (`3mf.cpp:904`). Round-trips automatically, identical to layer orientations.
  - Added `print_group` to `PrintObject::invalidate_state_by_config_options` (`PrintObject.cpp`): changing it now invalidates only `psSkirtBrim` + `psWipeTower` + `psGCodeExport` (mirrors `print_sequence`) instead of falling through to the `else` branch that did a full `invalidate_all_steps()` (wasteful reslice).

## Current Status: CRASH FIXED

The ByGroup `process_layer` SIGSEGV is **resolved**.

- **Crash (fixed)**: resolved `0x...74a1` via `addr2line`/`objdump` to `GCode::process_layer +0x141`, faulting at `mov (%rax),%eax` = `first_extruder_id = layer_tools.extruders.front();`. The empty-check passed but the deref faulted → `layer_tools` was a **dangling reference**. Root cause: the ByGroup main loop (`GCode.cpp:3797`) reused the previous group's `ToolOrdering` when `tool_ordering.first_extruder() == final_extruder_id` — a guard copied from the ByObject pattern. In a single-extruder print `first==last` always, so every group after the first reused group 1's `ToolOrdering`; `tools_for_layer(group2_z)` then indexed past group 1's `m_layer_tools` and returned garbage. **Fix**: rebuild `ToolOrdering` unconditionally per group (still seeded from `final_extruder_id` for tool-change continuity).
- Earlier crash 1 (multi-object `ToolOrdering` constructor) was a real prerequisite fix; the address `0x...70e1` was from a different ASLR run and maps elsewhere — disregard that exact symbol.

### Verification still needed
- Rebuild (`cmake --build build --config RelWithDebInfo --target UnlayeredSlicer`) and slice a multi-group plate (single-extruder) to confirm no crash + correct grouped output.

## Orientation preservation within a group (fixed)

Symptom: with ByGroup, only the first object's orientation was applied; other group members printed at the wrong orientation.

Cause: extrusion coordinates are baked into each object's `post_trafo_inverse()` frame inside `process_layer` (the per-island `trafo` at the `extrude_perimeters`/`extrude_support` calls). On a B/C-axis machine that frame is only correct if the machine is *physically* oriented with the same trafo (`trafo_to_rotation_gcode`). ByObject emits `trafo_to_rotation_gcode(obj)` + `set_last_trafo(obj)` per object. The ByGroup code only oriented once per group (to `first_obj`), so every other interleaved group member had a mismatched physical orientation.

Fix (two parts in `GCode.cpp`):
1. **Per-instance re-orientation inside `process_layer`** (per-instance loop, right after `m_layer` setup): when `print_sequence == ByGroup` and the instance's `post_trafo_inverse()` differs from `last_trafo()`, emit a Z-lift + `trafo_to_rotation_gcode(obj_trafo)` and `set_last_trafo(obj_trafo)`. Guarded by `isApprox`, so a uniform-orientation group rotates once and repeated passes (wiping / multi-extruder) are no-ops.
2. **Between-group transition** now rotates back to **Identity** (not `first_obj`) and `set_last_trafo(Identity)`, so the identity-frame travel-to-origin is physically correct and `process_layer` owns all per-object orientation. Removed the now-unused `first_obj` local.

Caveat / possible follow-up: this lifts 25 mm and rotates on every orientation change *within each layer*, which can mean many rotations per layer for mixed-orientation groups. Correct but motion-heavy; a smaller lift or smarter rotation sequencing could be a later optimization.

## Notes

- Wipe tower: ByGroup disables it (`has_wipe_tower = false` at `GCode.cpp:3102`), same restriction as ByObject.
- Timelapse gcode: `GCode.cpp:5526` now includes `ByGroup` alongside `ByObject` to pass `m_printed_objects` for occlusion avoidance.
- `printing_by_object_gcode` hook fires at the start of each group (`GCode.cpp:3821`).
- `sequence_by_layer` flag at `GCode.cpp:5049` includes `ByGroup` so within-layer behavior is identical to ByLayer.
- NDEBUG **is** defined in RelWithDebInfo builds — asserts are no-ops, crashes are real memory faults.
- `GridColType` enum values **must** match push_back order in `init_cols`; mismatches cause wrong column data to be loaded, leading to null-pointer crashes in `construct_object_configs`.
