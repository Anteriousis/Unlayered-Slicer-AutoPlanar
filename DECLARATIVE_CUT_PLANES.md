# Declarative Cut Planes — Non-destructive Multiplanar (Phase 0–1)

## Why this exists

The original multiplanar workflow ("Cut Chunks") physically **splits** a `ModelObject` into
upper/lower clones on every cut, drops the clones to the bed, tags the upper piece with
`layer_rotation_b/c`, and relies on config inheritance + empirical fixups to reconstruct the
relationships afterward. An end-to-end audit of that path found a large class of bugs that all trace
back to the destructive split:

- upper/lower classification tags the wrong piece (or both, or neither) on angled/side cuts;
- `auto_drop = false` is a no-op, so pieces snap to the bed and lose their spatial relationship;
- the lower piece is never tagged and depends on fragile inheritance;
- cut tags are written outside the undo snapshot;
- differently-oriented segments are not guaranteed to physically join, because slicing and G-code
  reconstruct positions from post-drop offsets rather than one shared frame.

**Declarative cut planes** remove the root cause: the object is *never* split in the user's model.
Cut planes become non-destructive per-object annotations (a sibling of `layer_config_ranges`), and
the mesh partition + per-segment print orientation are derived at slice time from the full set of
planes, in one canonical object frame.

This document describes the foundation landed so far. It is the first two phases of the design
proposal; later phases (authoring gizmo, slice-time segmentation, sequencing, collision) build on it.

## Data model (`src/libslic3r/Model.hpp`)

```cpp
struct CutPlane {                    // one plane, object-LOCAL frame
    Vec3d point  = Vec3d::Zero();    // a point on the plane
    Vec3d normal = Vec3d::UnitZ();   // unit normal = print direction of the far-from-bed side
};                                   // B/C are DERIVED downstream, never stored (avoids drift bugs)

struct CutPlaneStack {               // the non-destructive set attached to a ModelObject
    bool                  enabled = false;
    std::vector<CutPlane> planes;    // order matters twice: seat-rule tie-break, and siblings print in REVERSE authoring order
    bool active() const;             // enabled && !planes.empty()  — what slicing will key off
};
```

`ModelObject` gains one member, `CutPlaneStack cut_planes;`, plus a `has_cut_planes()` helper.
Design choices:

- **Normal is the single source of truth.** The far-side print direction is the plane normal; the
  machine B/C angles are computed from it at slice/export time and never stored, so there is no
  "stored angle vs. moved geometry" drift.
- **Object-local coordinates.** Planes live in the object's own frame, so they survive move/rotate/
  scale of the instance without needing per-edit correction.
- **`enabled` is separate from presence.** A user can toggle the whole behavior off for an object
  without discarding the authored planes.

## Persistence

Two independent paths, both modeled exactly on `layer_config_ranges`:

1. **Cereal** (`Model.hpp` `save`/`load`): `cut_planes` is appended to the `ar(...)` list, and
   copied in `ModelObject` copy/move assignment (`Model.cpp`). This covers in-session undo/redo and
   the app's model backup. Appended at the end of the archive list (same convention used for
   `cut_connectors`/`cut_id`).
2. **`.3mf` project file** (`src/libslic3r/Format/bbs_3mf.cpp`): a new `Metadata/cut_planes.xml`
   part with a dedicated importer (`_extract_cut_planes_from_archive`) and exporter
   (`_add_cut_planes_file_to_archive`), indexed by 1-based object index, mirroring the layer-config-
   ranges importer/exporter, archive-scan hook, reset, and import-time assignment. XML shape:

   ```xml
   <objects>
    <object id="1" enabled="1">
     <plane px=".." py=".." pz=".." nx=".." ny=".." nz=".." />
    </object>
   </objects>
   ```

Backward compatibility: ordinary objects have an empty stack and write nothing, so existing projects
are unaffected. The legacy destructive-cut path (separate objects carrying `layer_rotation_b/c`) is
left intact and still slices, so old projects keep working while new objects use `cut_planes`.

## Implemented: authoring, segmentation, slicing, sequencing (Phases 2–4)

The phases above the foundation have since landed:

- **Data model additions**: `CutPlane` gained `build_normal`/`has_build_normal` (per-segment build
  direction override; unset = track the plane normal) and a per-plane `enabled` flag. The 3mf part
  writes `bnx/bny/bnz` and `enabled` attributes only when non-default, staying compatible both ways.
- **Shared partition module** (`src/libslic3r/CutPlaneSegmentation.{hpp,cpp}`): the
  arrangement-cell (sign-vector) segment model. Planes are normalized so the bed-resting cell is
  all-near (a plane is flipped only when its far side reaches *strictly* lower; ties keep the
  authored orientation). Cells are sampled from mesh vertices plus edge-span midpoints, with
  distance-zero "touch" attribution at plane crossings so base-plane choice is right even when
  geometry rests exactly on a cut. Each non-root segment's base plane is the bounding plane it
  actually SITS on -- the one with the largest cut-face contact; a floating segment takes the
  closest far-side plane, and remaining ties keep plane order (2026-08-30: nearest-distance alone
  tied at ~0 on every bounding plane and degenerated to plane order, orienting far-of-several-planes
  segments wrongly). Each non-root segment has one base plane, a parent across it,
  and BFS-from-root print order -- siblings enumerate in DESCENDING plane order, so the
  first-authored cut prints last (2026-08-31). `segment_meshes()` cuts real per-segment meshes for the GUI
  preview from the *same* descriptors the slicer uses — preview and slicer cannot drift.
- **Duplicate planes**: a plane geometrically identical to an earlier one (either facing) is
  dropped at derivation -- the first authored wins (2026-08-31). Nearly-coplanar planes with a
  real offset make a sliver segment; thinner than the first layer, the whole feature goes inert
  (`SegmentTooThin`) with a panel note. Crossing planes are the ordinary multi-cell case.
- **Shared B/C math** (`Geometry::layer_rotation_matrix`, `bc_from_build_normal`,
  `build_normal_from_bc`): one home for the multiplanar conventions; the previous three
  copy-pasted rotation blocks and the gizmos' inline formulas now call these.
- **Slice-time segmentation** (`PrintApply.cpp`, `PrintObjectSlice.cpp`): `Print::apply` emits one
  internal PrintObject per segment, keyed by `(trafo, segment_index)` — gated on
  `enable_nonplanar && cut_planes.active()`, byte-identical legacy behavior otherwise. The mesh is
  never cut in the slicing path: each layer's ExPolygons are intersected with the planes' 2D
  half-plane traces (equivalent to clipping the solid, with no cap triangulation and no mesh
  copies). Root segments slice along +Z exactly; non-root along their world-frame build normal
  (inverse-transpose of the instance linear part — correct under scale/mirror). Leading empty
  layers are stripped; elephant foot / z-contouring / brim / skirt / first-layer islands are
  root-only; ConflictChecker skips non-root segments.
- **Sequencing**: segments auto-group per object (implicit negative `print_group` ids; a user-set
  group wins), and within a group layers order by `(segment BFS rank, print_z)` with no
  epsilon-merge across ranks. `Print::validate` requires the By-Group print sequence when
  segments are present.
- **Skeleton authoring** (2026-08-31): the Skeleton gizmo's "Cut by Skeleton" / "Cut here" author
  planes into this same stack (one per skeleton edge: object-local midpoint, smoothed tangent as
  the plane normal, no build_normal override) instead of destructively splitting the object;
  capped at 64 planes (uint64 cell mask); "Cut by Skeleton" then opens the Cut Planes gizmo. The
  destructive per-edge path (arm/trunk identification, layer_rotation_b/c writes, object
  reordering) is gone from this gizmo; legacy Cut Chunks keeps its own.
- **Authoring GUI** (`src/slic3r/GUI/Gizmos/GLGizmoCutPlanes.{hpp,cpp}`): a new gizmo (subclass of
  `GLGizmoCut3D`, toolbar entry `EType::CutPlanes`) edits `ModelObject::cut_planes` in place.
  The model is the source of truth; drags move only the gizmo pose and commit on drag-end under
  undo snapshots. Plane list panel (select / per-plane enable / flip / delete / add), numeric B/C
  build-direction editor (world-frame machine angles; storage stays a normal vector), inactive
  planes rendered dimmed with per-plane contours, click-to-select, and a "Preview chunks" toggle
  showing the implied segments in distinct colors (`cut_segment_color` palette). Object list shows
  a clickable "Cut planes" info badge. All local↔world conversions use the full instance matrix
  (points by `M`, normals by `(M.linear()⁻¹)ᵀ`) — NOT the offset-only `get_cut_matrix()`.
- **Select face** (`GLGizmoCutPlanes::update_face_hover` / `align_plane_to_hovered_face`): a panel
  button arms a pick mode; the next click on the model snaps the editing plane to the clicked
  surface (hit point, patch normal) and commits it to the bound stored plane under an undo
  snapshot. The pick raycasts the gizmo's merged object-local mesh, so any face qualifies (Lay on
  Face only offers convex-hull facets). Facets are grouped into connected coplanar patches
  (`compute_face_patches`, Lay on Face's normal tolerance); the whole patch under the pointer is
  highlighted and the largest patches are drawn as faint hints while picking. Because the base's
  "placed outside" warning reads an object clipper this gizmo never renders, `has_valid_contour`
  is virtual and answered from the active-pose slice, counting a plane that lies exactly on a flat
  face as touching.
- **Per-part build direction -- part segments** (`CutSegmentDescriptor::part_volume`,
  `append_part_segments`, `segment_includes_volume`): a model part whose own config carries
  `layer_rotation_b/c` (the same world-frame machine angles an object uses; they round-trip through
  the 3mf and undo like any per-part setting) is sliced as its own segment along that direction.
  Such a "part segment" is the part's whole mesh with no clips and no parent, never the root (no
  brim, no support cascade -- like a legacy per-object chunk), and it is left out of the plane
  partition: the root and the plane segments are derived from the undirected parts only, and an
  object with directed parts but no planes gets a plain root over the undirected parts plus one
  segment per directed part. In slicing every segment shares the object's volume list; a segment
  simply keeps no material from parts it does not own (negative volumes and modifiers stay
  everywhere). `Print::apply` segments an object when it has active planes OR directed parts. The
  gizmo authors it with "Use plane as build direction" while a single part is selected, and the
  chunk colours show each directed part in its own segment colour.
- **Part scope** (`GLGizmoCutPlanes::scoped_volume`): the gizmo also opens on a single selected
  part of a multi-part object. Everything mesh-derived -- contours, the editing surface, face
  picking, the chunk preview -- is then built from that part alone (object-local, under its own
  matrix) and the preview hides only that part, so the user sees how the planes cut this body.
  The plane stack and the object build direction remain per object; the panel says so. The
  scoped preview is therefore a view of one body, not the slicer's whole-object partition.
- **Object build direction from the plane** (`GLGizmoCutPlanes::render_object_build_direction`):
  the panel shows the object's per-object `layer_rotation_b/c` (Others ▸ Multiplanar) and a
  "Use plane as build direction" button that writes them from the editing plane's world normal
  via `Geometry::bc_from_build_normal` under a snapshot, plus a reset. Combined with Select face
  this sets a whole object's slice direction from one of its faces -- the way to give each body
  of an assembly of separate objects its own direction. One direction per object (parts share
  it), and it is not used while the object's cut planes are enabled, which the panel says.
- **Chunk colours outside the gizmo** (`GLCanvas3D::_update_cut_chunk_previews`,
  `GLVolume::render_cut_chunks`): in the Prepare view every instance of an object with an active
  stack is drawn one colour per chunk (same palette, same `derive_cut_segments` call as the gizmo
  and the slicer) in place of its model parts, whether or not the gizmo is open. The chunk models
  are cached per (object, instance) and rebuilt only when the stack, the instance pose, the
  thickness bound or a part's mesh/pose changes; the real volumes stay in the scene, so picking,
  selection and gizmos are untouched. The gizmo's own preview still wins while it is open (it hides
  the object's volumes). App preference `show_cut_chunk_colors` (default on; View ▸ Color Cut
  Chunks, Preferences ▸ Graphics) turns it off, and the first time a plate with such an object
  uses a second filament the user is offered, once per session, to turn it off, since the chunk
  colours hide the filament colours (`no_ask_cut_chunk_colors_multi_filament` remembers "don't
  ask again").

Still to come:

- **Phase 5:** authoring-time rotary-limit validation; obstacle-aware, bounds-clamped lifts and
  collision handling.
- **Kinematics fixes (orthogonal):** honor `axis_selection`, MMU/wipe handling in grouped mode,
  remove debug strings/dead code.

Segment-aware support generation (implemented; see `Support/SegmentSupportBounds.{hpp,cpp}` and
`Print::run_segment_support_cascade`):

- Each segment's support is confined to its own half-space and terminates on its base plane, which
  is generally a TILTED floor rather than a flat plate, so support may legitimately extend below the
  segment's slice-frame z=0.
- Segments of one object under one instance transform form a family, generated child-first. What a
  child cannot land on its own floor is handed to its parent as forced overhang and carried onward,
  so load reaches the build plate through the whole chain. A parent may receive from several
  children; contributions are unioned.
- All three styles -- normal/grid, classic tree and organic tree -- receive and act on inherited
  demand. Root-only brim/skirt is unchanged, and first-layer base expansion is suppressed for
  non-root segments (their first layer is a cut face mid-stack, not a plate).
- A segment that FLOATS above its base plane -- reachable when a plane passes through empty space
  between disconnected shells, so no cut face is produced -- has its first layer seeded explicitly
  as unsupported; the stock generators assume layer 0 rests on the ground and would support none of
  it. **Normal/grid support only** (see below).

How each style reaches the floor:

- **Normal/grid** synthesises intermediate layers below the segment's zero, down onto the floor.
- **Both tree styles** plan their branches from non-negative layer indices -- classic's node graph
  stops dropping at object layer 0, organic starts its lowest boundary at 0 -- so branches alone can
  never reach a floor below zero. Their feet are therefore carried down as a straight column, with
  their own extrusions, after the branch work is done: the same physical answer as normal support
  for the last stretch onto the cut face, without reworking either node graph. The column is also
  seeded with demand landing on object layer 0, which neither node graph can turn into contacts, so
  a floating segment is supported under every style. Organic additionally returns before assembling
  any layer when there is no natural overhang at all -- exactly the floating case -- so the column is
  applied after it returns rather than inside it.
- A **raft** on a non-root segment skips the column: the first support layers are treated
  positionally as raft, and prepending below them would reassign that mapping.

Not supported by either segment: a cut plane lying in OPEN SPACE with material neither above nor
below it nearby. A segment's support cannot extend past its own base plane, and a parent cannot
build above its own top layer, so the span between the plane and the parent's material belongs to
neither. A plane cutting through material -- the ordinary case -- has no such span.

Support limitations, by design in this pass:

- **Demand landing on a non-parent far-side face is dropped.** Every far-side clip is load-bearing
  (a downward-V segment really does rest on two cut faces, which is why `floor_z()` takes the max
  over all of them), but only the `base_plane` neighbour is the cascade parent. Equally true of the
  original world-frame implementation.
- **Near-vertical interfaces (~90°)** fall back to seeding a single contact height rather than
  distributing across a tilted band; branches crossing such an interface are not routed around
  upper segments. See the tunnel-crossing memo -- out of scope here.
- The half-plane clip is realised against a fixed 10 km box, inherited from the original helper.
- Cascade performance is unchanged from the original: serial within a family, with per-layer
  Clipper work over tall demand columns.
- **A segment's layer schedule is phase-shifted when a plane does not fall on a layer boundary.**
  The slice frame is rebased to the segment's exact material minimum, while the schedule is rebased
  to the lower boundary of the first full-object layer overlapping it. A cut at 10.13mm through a
  0.2mm grid therefore keeps the object's sequence of layer heights but shifts the boundaries by
  0.13mm. Height ranges are looked up through `PrintObject::segment_schedule_base()`, which is
  exact for a segment building along +Z; for a tilted segment one slice plane spans a range of
  object heights, so the lookup uses a representative height.

Known limitations (documented in code): a cell occupied by volume but touched by no mesh edge
(e.g. a bounded sliver formed by 3+ planes inside one large face) is missed; configurations with
no all-near root cell (e.g. two vertical planes whose shared-near quadrant is empty) disable the
feature for that object with a log warning; at most 64 enabled planes per object.

## Verification

- Unit tests: `tests/libslic3r/test_cut_plane_segmentation.cpp` (segment derivation incl. chains,
  branches, flipped normals, touch attribution, half-plane clipping, volume conservation, B/C
  round-trips) and `tests/fff_print/test_cut_plane_apply.cpp` (apply-level identity/reuse/gating,
  slice-level complementary geometry, root-only brim).
- Run: `ctest --test-dir build/tests/libslic3r` / `build/tests/fff_print/fff_print_tests "[cutplanes]"`
  (configure with `-DBUILD_TESTS=ON`).

- **Print sequence default.** The Unlayered process bases keep `"print_sequence": "by object"`
  (a by-group default was tried and reverted in review). A print that contains segments therefore
  fails `Print::validate` with *Declarative cut planes require the "By group" print sequence.* until
  the user switches the process setting -- deliberate, so ordinary prints keep their sequencing.

- **Legacy `layer_rotation_b/c` chunks keep the identity support frame.** Un-rotating support with
  the body (as segments do) is only correct when the support was generated against a floor: segments
  get that from `SegmentSupportBounds`, legacy chunks do not -- theirs grows to the slice-frame z=0,
  a plane tilted with the chunk, and un-rotated it reached machine z = -9.4 mm on a 30-degree test
  chunk. `GCode.cpp` therefore applies `post_trafo_inverse()` to support for `is_segment()` objects
  only (measured 2026-08-29; the pre-merge output for such chunks is byte-identical apart from arcs).
- **Low-z guard.** `GCodeWriter::extrude_to_xyz` throws only when a point is below 0.1 mm in BOTH the
  slice and the machine frame: a legacy chunk standing tilted on the bed has first-layer wall points
  at machine z ~ 0 (the tilted slice meets the bottom face along a line) and such prints have been
  made; a segment's rebased negative slice z still passes when the nozzle is really above the bed.

### Hotend collision check (post-slice, multiplanar only)

`GCode/HotendCollisionCheck.{hpp,cpp}`, run from `Print::export_gcode()` after `GCode::do_export()` when
`enable_nonplanar` is on, the print has more than one tool axis and checks are not suppressed
(`--no-check` disables it together with the path-conflict check). The hotend is a cone with its apex
at the nozzle tip and its axis along the tool axis -- printer settings `nonplanar_hotend_cone_height`
(40 mm) and `nonplanar_hotend_cone_angle` (90 = a 45-degree half-angle; 0 = off). Material printed in
an EARLIER tool-axis episode (a chunk or segment) that lies inside the cone while a later one prints is
a collision. Frame: RRF compensates, so the processed move positions are the tip in the bed frame and
material stays put; the axis per move is `GCodeProcessorResult::tool_axes[move.tool_axis_id]`.
The result is a `ConflictResult` with `message` set, so it surfaces exactly like a G-code path conflict:
the preview's serious-warning notification in the GUI, exit -101 in the CLI (checked again after export,
since the path-conflict check runs before it). Cost measured at 7-133 ms on the test fixtures.
Limitation: the table swing itself is not modelled -- moves before a `; TOOL_AXIS` tag are tested under
the previous axis, moves after it under the new one. `tools/gcode_collision_check.py` is an independent
numpy re-implementation over the exported G-code, used to cross-check the C++ result.

### Regression protocol: the merge must not move a planar print's bytes

`tests/fff_print/test_gcode_baseline.cpp` (`[.][baseline]`, hidden) is a *manual* differ -- it
writes canonicalised G-code to `$BASELINE_DIR` and only asserts reproducibility; there is no golden
file in the repo. The A/B that verified the 2026-08 merge therefore compared two *binaries* on the
same inputs (scripts under `build/ab/`: `run_ab.py`, `canon.py`, `compare.py`, `assert_c1.py`,
`flatten.py`, `patch_3mf.py`):

- Canonicalise exactly as the baseline test does: drop `; generated by` lines, `id:<n>` -> `id:*`.
  Object ids are per-run, so raw sizes differ by a few KB between identical runs; canon files do not.
- Fixtures: a 20 mm cube (plain / by group / `--enable-arc-fitting`), an overhang with
  `--enable-support`, real GUI projects with embedded configs (web-app 3mfs, a multi-object MK3
  project), a two-cube 3mf with `layer_rotation_b=30` on one object (legacy chunks), and a tall box
  with `Metadata/cut_planes.xml` on and off.
- Expected differences on a rotated legacy chunk are exactly: `G2/G3` become `G1` (an arc in a tilted
  plane is not an XY-plane arc); its support keeps the pre-merge identity frame (see above).
- Planar output is compared by GEOMETRY, not bytes: since the 2026-08-29 fixes an extrusion carries
  a Z word only when Z changes (as upstream), the bed waits with M190, and the sequential
  transition is upstream's on a non-5-axis machine -- so `compare_xye.py` (ordered X/Y/E of every
  extrusion) must report identical and `check_layer_z.py` (every extrusion at its `;Z:` layer)
  must pass; the "empty layer between 0 and z" warning must not appear at all.
- CLI notes that cost time: `--load-settings` does not resolve `inherits` (flatten first); boolean
  options are bare flags (`--enable-support`, not `--enable-support 1`); `--no-check` must precede
  `--slice`; a 3mf whose config carries `post_process` (even `[""]`) is refused with -19; the exe is
  a GUI-subsystem shim, so read `00000.log` in the CWD rather than stdout.
- Hand-authoring `Metadata/cut_planes.xml`: `object id` is the **1-based index into
  `model.objects`**, not the 3dmodel object id; point/normal are object-local, so `pz="0"` cuts
  mid-height. A CLI-exported 3mf also needs `filament_settings_id` in its project config.
