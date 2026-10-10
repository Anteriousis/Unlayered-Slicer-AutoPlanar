# Belt printer

A belt printer prints onto a conveyor belt under a gantry tilted by a fixed angle (usually 45
degrees). Layers are parallel to the gantry, so every layer is a plane tilted against the belt,
the print grows along the belt, and the machine's own axes are not orthogonal to the bed frame.
This document describes how the fork models that, and why the pieces sit where they do.

## The two stages

A belt printer is the single-segment case of the fork's multi-planar slicing. An ordinary object
on a belt printer is sliced rotated by the belt tilt, exactly as a cut-plane segment is sliced
rotated by its build normal, and the G-code stage undoes that rotation per move. What a belt adds
is one more, global stage after that:

```
slice frame ──[stage 1: PrintObject::post_trafo_inverse(), per move]──▶ bed frame
          ──[stage 2: MachineKinematics::to_machine(), per emitted word]──▶ machine coordinates
```

- **Stage 1** is the existing per-object un-rotation applied inside `GCodeWriter` from the
  `trafo` argument of `travel_to_xyz` / `extrude_to_xyz`. `GCodeWriter::m_pos` is therefore the
  **bed frame**: X across the belt, Y along it, Z above it. Every geometric decision in the
  G-code stage (first-layer bands, the very-low-Z guard, the plate clip) is made in this frame.
- **Stage 2** is `MachineKinematics` (`src/libslic3r/GCode/MachineKinematics.hpp`), a strategy
  owned by the writer and consulted at every word-emission site. `CartesianKinematics` (the
  default) applies the `gcode_remap_*` axis mapping; `BeltKinematics` applies that mapping and
  then `MachineFrameTransform`: a shear of cot(a) coupling the height axis into the belt-feed
  axis and a 1/sin(a) scale on the gantry axis, both derived from the tilt angle. It reports
  `must_emit_all_axes()` (so no site may omit a word under a coupling map),
  `suppress_lift_at_unknown_position()` and `supports_arc_moves()` (false: a circle is an
  ellipse in machine coordinates).

The two stages are never folded together. `Eigen::Transform::rotation()` — which stage 1 uses —
returns the polar-decomposition rotation and would silently discard a shear. Stage 2 is applied
to the emitted point only.

The kinematics are installed on the existing writer in `GCode::_do_export`, never on a fresh
one, so plate offsets and the rest of the writer state configured before that point survive.

## Configuration

Two angles about the printer's tilt axis matter, and they are separate settings on purpose:

- **The machine's tilt**, printer level: `belt_frame_tilt_angle`, the physical angle between the
  gantry and the belt. It drives the machine-frame mapping (stage 2), the rotary home and the
  preview's nozzle, through `BeltTransformPipeline::build_gantry_matrix()`.
- **The print's slicing angle**, process level: `belt_slice_rotation_angle`, the angle between the
  layers and the belt, shipped at 45 and tunable per print. It drives the slice frame through
  `build_rotation_matrix()` and `layer_rotation_trafo(print_config, object_config)`, the single
  helper both `PrintObject::update_slice_frame()` and `PrintObject::update_slicing_parameters()`
  derive from (deriving them separately once sized the schedule to the unrotated height). The
  same helper tells `slices_to_regions` that the frame is rotated: a multi-part object then takes
  the cut-plane segments' route and decides which part is present at a layer from the slices
  themselves, since the per-volume bounding boxes are z-extents in the unrotated instance frame
  (consulting them cut a two-part Benchy off at its untilted height).

A belt printer prints a somewhat misaligned layer plane fine, a grossly misaligned one does not,
so the printer also carries `belt_slice_angle_tolerance` (default 10) and `Print::validate()`
refuses a slicing angle further from the tilt than that. Stage 1 undoes whichever angle the print
was sliced at, so stage 2 never needs to know it. The other printer keys keep OrcaSlicer's names,
values and defaults so belt profiles carry over: `belt_printer`, `belt_printer_infinite_y`,
`belt_slice_rotation` (the tilt axis; only X is offered outside developer mode), `gcode_remap_x/y/z`
(the belt convention is Y = gantry, Z = belt feed, often with X reversed) and the contact-aware
first-layer keys of the next section. A printer preset written when the slicing angle was a
printer key has that value handed to `belt_frame_tilt_angle` on load (`handle_legacy_composite`).

Not carried over from Orca: its `BeltBackTransform`, `BeltSliceStrategy`, `on_set_origin`, the
`*_global` / `preslice_remap_*` / `gcode_back_transform` keys and the `belt_frame_tilt_decouple`
flag. The first exist there to decide which stage owns the object's origin; here stage 1 owns
placement and stage 2 owns the machine. The flag is unnecessary once the two angles are two
settings.

## Support

The belt is the surface support must end on, and in the slice frame it is a tilted plane — the
same situation as a non-root cut-plane segment resting on its cut face. `SegmentSupportBounds`
is therefore active for a belt object with one floor, the plane `PrintObject::bed_plane_in_slice_frame()`
returns, which is also the plane the slicer clips layers with, so support and geometry can never
disagree about where the belt is. `PrintObject::support_floor_is_tilted()` is the predicate the
generators use where they previously asked "is this a non-root segment": the first-layer base
expansion is skipped (the first slice layer is the leading tip, not a plate footprint), the
classic tree carries its lowest feet down onto the floor, and the G-code stage un-rotates
support like the object. Columns grow along the slicing normal, so on a 45 degree belt an
overhang's support foot lands a distance equal to the overhang height behind it along the belt,
unless the object's support direction (`support_direction`, see `support-direction.md`) leans it:
"World vertical" resolves to the belt normal in the slice frame, so the column drops straight onto
the belt under the overhang. The belt's floor plane goes through the same shear as a segment's
bounds while support is generated in the sheared frame, so a leaning column still ends on the
belt. On a 45 degree belt that lean equals the default `support_direction_max_lean`, inside its
tolerance.

### The floor band

Overhang detection compares a layer with the one below it; on a tilted layer the strip that rests
on the belt has nothing below it in the slice frame and read as an overhang, sprouting support and
overhang perimeters along every leading edge. `SegmentSupportBounds::floor_band(z_lo, z_hi)` is the
strip of the floor plane that crosses the slab between two layer planes; it is unioned into the
lower layer's polygons wherever "what is below" is asked: `TreeSupport::detect_overhangs`,
`SupportMaterial`'s contact search and `LayerRegion::make_perimeters` (through
`Layer::lslices_with_floor`). Cut-plane segments use the same band for their cut face, so a
segment on a planar printer gets it too. The organic tree carries its lowest feet down onto the
floor from `support_islands`, the classic tree from its own footprint.

`brim_type = leading_edge_only` is a belt brim type; on a flat bed it falls back to an ordinary
outer brim.

## Contact-aware first layer

"First layer" is not a layer index on a belt: the strip of **every** slice layer that lies on
the belt is a first layer, and a non-root cut-plane segment's slice layer 0 lies on plastic, not
on the bed. The G-code stage therefore decides first-layer treatment per path by **what the
material rests on and how far above it the path lies** (`GCode::classify_path_contact`):

| class | surface | height |
|---|---|---|
| `Bed` | the build surface, bed frame z = 0 (bed and belt alike) | bed-frame z of the path |
| `Object` | the cut face a non-root segment rests on (`SegmentSupportBounds::floor_z`) | slice z minus the floor's z under the path |
| `None` | neither | — |

The heights are computed in the frame `_extrude` already works in, so no machine coordinate is
ever parsed back. The band for the boolean is the initial layer height; the ramps
(`close_fan_the_first_x_layers`, `full_fan_speed_layer`, `slow_down_layers`,
`object_contact_layers`) count bands of `first_layer_plane_thickness` (-1 = the initial layer
height) above the surface, so their numbers keep their meaning. A path that crosses a band level
is written as its pieces, each uniformly classified; cuts are made only where the class changes
so a run inside one class keeps its original vertices.

Classification is active only for a rotated frame or a non-root segment. A planar object keeps
the legacy layer-index rule, which is exact there, so planar output is unchanged. `first_layer_plane`
(Orca's key) set to `xy` forces the legacy rule everywhere; its other values behave as `auto`.

What the class drives: the initial-layer speeds, flow ratio, acceleration and jerk, the small-area
flow-compensation gate and the overhang-detection gate inside `_extrude`; the initial-layer travel
speed in the writer (`GCodeWriter::set_first_layer_band`, tested against `m_pos.z()`); and the
fan through inline markers (`;_CONTACT_BED_START:<band>` / `_END`, `;_CONTACT_OBJECT_START:<band>:<mode>:<speed>`),
which `CoolingBuffer` applies with the same first-N-layers rule it uses per layer
(`CoolingBuffer::compute_main_fan_speed`) but with the band index in place of the layer index.

Material on printed plastic has its own controls, process level: `object_contact_layers`,
`object_contact_fan_mode` (same as first layer / keep the layer's fan / off / fixed),
`object_contact_fan_speed`, `object_contact_speed` and `object_contact_flow_ratio` (0 = the
first-layer value). The defaults reproduce the first-layer rules, which is what a segment's
first layer received before; acceleration and jerk on plastic stay at their normal values. In
the process tab they form the "Multiplanar cut plane settings" group on the Speed page, shown
only while the selected printer has non-planar printing enabled (or in developer mode), since a
planar printer never lays a layer on a cut face.

Temperatures stay per layer. On a belt the initial nozzle and bed temperatures are held for
`belt_initial_temperature_length` mm of belt travel (0 = the whole print), measured as the
layer's slice z divided by sin(tilt).

## Copies of an object

Every belt instance is its own `PrintObject` (the schedule is per instance), and so is every copy
of a multi-planar object with its own B/C rotation on a planar printer. The reason is the
un-rotation pivot: `PrintObject::update_slice_frame()` derives the pivot and the z floor from the
object's *own* instance placement when the ModelObject has several copies, because the
ModelObject's bounding box spans all copies and a pivot taken from it shifted every copy by the
spread between them. A single copy keeps the bounding-box derivation, which is identical to well
under a micron and keeps planar output byte-identical.

An object dropped below the plate is clipped at the belt like a multi-planar object at its base
plane (`bed_clip` in `slice_volumes`), so the sunk part is never printed.

## Several objects: the belt schedule

The belt only advances, so material at a smaller belt position must never be printed after
material at a larger one, whichever object it belongs to. Every instance on a belt is its own
`PrintObject` (keyed by the instance id in `PrintApply`), and `Print::update_belt_schedule()`
raises each object's slice frame by how far its lowest point lies beyond the lowest point of the
whole print along the gantry normal (exact mesh minima). `print_z` then measures the same belt
position for every object, the ordinary by-layer merge interleaves them in belt order, and the
per-move un-rotation subtracts the offset again so coordinates are unchanged. The leading empty
layers of a trailing object are dropped by the layer collector. Moving an instance along the belt
changes its offset and reslices that object. Several objects therefore require the by-layer
sequence; by-object or by-group would drive the belt backwards between objects.

## Multi-planar belt

Cut-plane segments compose with the belt. The root segment builds along the gantry normal:
`Print::apply` hands `derive_cut_segments` that direction, so the root's extent, its slice frame
(`effective_layer_rotation_bc`) and its support floor (the belt plane, through
`bed_plane_in_slice_frame`) all follow it, while non-root segments keep their own normals and
cut-face floors. The root's extent is a projection on the gantry normal from the instance origin,
so part of any object projects negative without being sunk: unlike a flat-bed root, which keeps
its sinking (`CutSegmentDescriptor::slice_z_base`), a belt root is rebased onto its own minimum
and whatever lies under the belt is removed by the belt clip. The belt also bounds the support of
every non-root segment, on top of its cut faces, since a cut face extended past the part can run
on under the belt. The rotary words describe the tool axis relative to the gantry
(`GCode::m_world_to_gantry`), so the root sits at the rotary home and a segment tilted against
the gantry gets the same B/C it would get against a flat bed; the virtual-5-axis tilt limit is
measured against the gantry normal likewise. A legacy per-object B/C composes on top of the belt
tilt the same way. Because a segment's layers are not parallel to the gantry, segments are
sequenced by group rather than by belt position and take no schedule offset, which is why a
multi-planar belt plate holds one object: a second object would need the belt to run backwards.

## Preview and bounds

A belt file's coordinates are machine coordinates, so nothing downstream of the writer may read
them as positions on the bed. `GCodeProcessor` builds `GCodeProcessorResult::belt_to_bed`, the
affine inverse of the very kinematics the writer used (sampled from `BeltKinematics::to_logical`
under the same remap and build volume), composed with the plate offset the processor adds to
stored positions and the Z origin a `G92` in the start G-code leaves behind. It is exact, so the
preview needs no anchoring against the model. The standalone viewer gets the same mapping from
the belt keys in the file's config dump.

The G-code preview maps every vertex through it at load time, which puts the toolpaths on the
model shells in the bed frame (the upright view). The hotend marker follows the gantry normal on
a belt without rotary axes (`GCodeProcessorResult::tool_axis_fixed`), as it stays vertical for
virtual-5-axis output: the physical nozzle does not follow the per-move tool axis there. Hotkey B, the legend checkbox and the canvas
toolbar menu switch to the raw machine-frame G-code for checking the transform itself; the switch
reloads the preview, since the mapping is baked into the vertices, and a belt file is exempt from
the viewer's same-result shortcut for that reason.

The bounds checks run in the bed frame too. The processor's plate and height check maps each
move and takes its height from the mapped Z, because `print_z` on a belt is the slice height
along the gantry normal and grows with the length of the print. The build volume treats Y as
open when `belt_printer_infinite_y` is set, both for the model and for the G-code, and the
canvas height warning compares the mapped toolpath top with `printable_height` (the clearance
above the belt) and stays silent in the raw view, where no comparison is meaningful.

## Painting on the belt

Colour, fuzzy-skin, support and seam paint is projected in the slice frame
(`PrintObject::object_to_slice()`, the very transform the layers were sliced with), not with the
unrotated instance transform, so on a belt the paint lands on the layers that cut the painted
facets. The general painted-line path therefore no longer subtracts the centre offset itself. The
top/bottom projection runs in the tilted frame too: the slices at a painted top edge are "top
surface" there, so the paint reaches a few millimetres further down the painted face than on a
planar printer.

## Purging on the belt

The classic wipe tower writes its own G-code text, which never passes through stage 2, so it
cannot exist on a belt (`Print::has_wipe_tower()` is false there). Its replacement is a sacrificial
prism: a real model object the plater keeps beside the parts (`GUI::ensure_belt_purge_tower`, run
on every background-process update and idempotent on its inputs), marked
`belt_purge_tower_object` and `flush_into_objects`, one wall, no shells, dense infill, one
disconnected island per simultaneous filament change. It is sliced like any object, so it picks up
the belt rotation; its width across the belt is `belt_purge_tower_width` (process), its height
follows from the worst-case purge volume of one layer through the tilted cross-section, and its
length follows the parts along the belt with ramp compensation at both tilted ends.
`enable_belt_purge_tower` (printer) turns it on.

`Print::_plan_belt_purge()` is the belt's psWipeTower step: it builds the tool ordering without
priming, drops the prism's layers above the last filament change (reversibly, since a wipe-tower
invalidation replans without reslicing), and marks the purge into the prism's extrusions with the
same flush-into-objects machinery the classic tower uses. A change on object B is purged into the
prism only if the prism has a layer at exactly B's print_z, so while the prism exists the belt
schedule rounds every object's offset to a multiple of the layer height: the grids coincide by
construction, the per-move un-rotation still undoes exactly the offset applied, and `validate()`
requires one layer height for all objects.

## Brim on the belt

The plate brim is a flat first-layer feature and stays off on a belt. Its belt replacement
(`BeltBrim.cpp`, ported from Orca) is a sheet laid onto the belt plane over many layers: each
slice layer touches the belt along a strip about layer height / tan(tilt) wide, so the object's
belt footprint is the union of those strips over all layers, and the brim region is offset from
it in a flattened metric where distances along the tilt axis are true on-belt distances.
`leading_brim_length` sweeps the region ahead of the part along the belt, `extra_brim_width` widens
it across the belt, and the brim type "Leading edge only" keeps only what lies ahead of the first
contact. Each layer then carries the one brim line whose band it crosses, with its height set to
the nozzle-to-belt clearance and its flow sized to fill the band. The bands ahead of the object's
first layer have no layer to ride on: they form an apron prologue of brim-only bands
(`BeltBrimBand`, deliberately not a `Layer`), generated in posSupportMaterial so their print_z
values exist before the tool ordering is built, merged into the object's layer list by
`GCode::collect_layers_to_print`, and emitted by `GCode::process_belt_brim_layer` with layer id 0,
so cooling treats them as first-layer material. Bands coinciding with an object layer are emitted
in the brim filament's pass before that pass's object material, once per layer and instance.

## Brim beads and the belt

A brim band's clearance is measured along slice z, but the nozzle's true gap over the belt is
that clearance times cos(tilt): 0.53 of the band height at 45 degrees for the 0.75 fraction the
single-line case uses. When a band is wider than the line pitch the beads go on the lattice, and a
lattice line that would land closer to the belt than that fraction is moved uphill to it, so no
bead is laid scraping the belt. The writer's "very low z" refusal scales with the first layer
height (`min(0.1 mm, 0.4 x first layer height)`) for the same reason.

The brim's filament is the object's outer-wall filament restricted to the filaments the writer
was handed: an object whose every extrusion is flushable (`flush_into_objects`) contributes no
filament to the tool ordering, and its brim then takes the current filament instead.

## Known limitations

- An apron layer shared by two objects whose brim filaments differ prints both aprons with the
  first filament.
- Height-range modifiers on a belt object are looked up in the raised, tilted frame, so a range
  authored in millimetres of object height matches a tilted slab, offset by the object's belt lead.
- A belt printer with a non-rectangular `printable_area` opens Y in the plate check but not in
  `BuildVolume`.
- The preview's per-line width and flow values (and the centre of gravity) are computed from
  machine-frame lengths; filament totals are exact.
- The Process-tab belt group and the printer-tab tilt-axis filter refresh on preset selection and
  mode change, not live when `belt_printer` is toggled.
- A user printer preset that inherits `belt_printer` from a parent and only overrides the old
  slicing-angle key is not migrated to `belt_frame_tilt_angle`.
- Contact-aware first layer on a cut-plane segment: the bonding layer is emitted at its top, one
  thickness above the floor, so with `object_contact_layers = 1` it is not classified as object
  contact and keeps the plain first-layer treatment. Widening the band changes the fan on that layer
  of every cut-plane print (the "as first layer" object fan mode does not honour
  `close_fan_the_first_x_layers`), so it is left for a follow-up.

## Restrictions

`Print::validate()` refuses on a belt printer: spiral vase, a second object next to a
multi-planar one, and several ordinary objects with any print sequence other than by-layer.
Skirt, the plate brim and the classic wipe tower are switched off: the first slice layer is the
leading tip, and the wipe tower emits raw text that bypasses stage 2 (the belt has its own brim
and purge prism, above). The build-volume height and
too-tall checks are skipped because slice z runs along the tilted normal.
