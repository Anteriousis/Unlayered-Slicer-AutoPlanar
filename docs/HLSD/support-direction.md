# Support direction

Support normally stacks straight along the slice frame's Z: each support layer is the layer
above projected down, so a column is always perpendicular to the layers. `support_direction`
(per object, in the Support page) lets the column lean along a direction that is fixed in the
printer's frame instead:

| value      | support axis                                                        |
|------------|---------------------------------------------------------------------|
| `layers`   | the layer normal (default, stock behaviour)                         |
| `vertical` | world +Z, whatever rotation the object or segment is sliced at       |
| `custom`   | the world direction given by `support_direction_b/c`, in the same B/C convention as `layer_rotation_b/c` (b = c = 0 is world vertical) |

The direction names a line, not a sense: support always grows from a segment's floor towards
its own +Z, so a world direction that maps below the slice frame's horizon (a segment printing a
downward-facing cut face) leans along the same line the other way.

The point is multi-planar printing. A cut-plane segment is sliced along its own build normal,
so its support leans with it; with `vertical` every segment's support keeps pointing at the bed,
across the whole stack, and the cascade that hands unsupported load from a segment to its parent
carries on along the same axis.

## How the lean is produced

None of the three generators (normal, classic tree, organic tree) knows about the direction.
`Support/SupportDirection.hpp` resolves the configured world direction into the object's slice
frame (`support_lean()`) and then, for the duration of `_generate_support_material()`,
`ScopedSupportShear` translates every layer's geometry in place by `-z · (u.xy / u.z)`, where `u`
is the unit support axis. That shear maps a column along `u` onto a vertical one, so the
unmodified generator, run against the sheared object, produces exactly the leaning support. The
scope then translates the object layers back and the generated support layers by the inverse
amount at their own `print_z`. A shear preserves z, so layers stay layers and an object/support
contact at a given height stays a contact.

Things that read object geometry during generation see the sheared frame consistently:

- `PrintObject::slice_support_volumes()` shears the painted enforcer/blocker slices the same way;
- `SegmentSupportBounds` composes the shear into the object-to-slice map, so a segment's cut-plane
  floors and ceilings are the same planes in the sheared frame, and the extension of support down
  to a tilted floor follows the lean;
- inherited cascade demand is sheared on entry and restored on exit; the footprint handed up to
  the parent is read from the un-sheared support layers.

`PrintObject::support_lean()` reports the lean only while the scope is alive, so nothing outside
support generation can mistake sheared geometry for the slice frame.

## Constraints and consequences

- **Overhangs are judged against the support direction, not the layer normal.** In the sheared
  frame a face's inclination is its inclination relative to `u`. This is the same choice the
  OrcaSlicer belt branch makes for its build-plate tilt, and it is what makes `vertical` mean
  "support what would fall".
- **The lean is limited by `support_direction_max_lean`** (45° by default). A leaning column
  overhangs itself by exactly the lean angle, so past 45° (`SupportLean::safe_lean_deg()`) it
  needs tuning and test prints, and slicing warns. A direction beyond the limit keeps its azimuth
  and is pulled back to the limit, with a warning. Tree branch angles add to the lean.
- The tree generators clip support to the bed outline as a single polygon; in the sheared frame
  that outline is off by the shear at height, so a leaning column near the bed edge can be trimmed
  a little early or late. Cut-plane segments never had a meaningful bed outline in their frame.
- Per-part (model volume) overrides of the direction are not plumbed: a part segment's
  `PrintObjectConfig` is the object's.
