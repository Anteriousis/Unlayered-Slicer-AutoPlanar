# Segment support cascade

A cut-plane partition slices each segment in its own frame and generates its support in that
frame, so support always ends at the segment's floor: its base plane, a cut face of an earlier
segment. Whatever the floor does not carry, the cascade hands down as *demand*, the load the
segments below have to build support for. Orchestrated by `Print::run_segment_support_cascade`
for each family (the segments of one model object under one instance transform), deepest
segment first.

## What is handed down, and to whom

A segment's footprint at its floor is the union of its lowest support layers and its lowest body
slices (`segment_support_footprint`). Body counts too: a segment that needs no support of its own
can still overhang the segment below.

The footprint is handed to **every segment printed before it**, not only to its parent (the
segment its base plane mostly rests on). Two planes crossing inside the object make a wedge whose
ceiling is a later segment's floor; that segment's parent is the root, so a parent-only handoff
skipped the wedge, and the root clipped the column at its own ceiling, exactly the wedge's slab.
Each receiver runs `distribute_demand`: the footprint is mapped into the receiver's slice frame,
kept only where it enters the receiver's floor/ceiling slab at the child's base plane
(`SegmentSupportBounds::on_plane_inside`; what lands above the receiver's ceiling reaches it
through the segment owning that slab, what lands below its floor never does), projected down
from the plane layer by layer, carved against the receiver's material on the way (a patch resting
on material is caught for good), and stored per layer as `PrintObject::segment_support_demand()`.
A receiver the column never crosses is skipped before the per-layer work.

The invalidation edge follows the same rule: a change to a segment's slices or support
invalidates the support of every segment below it (`Print::segments_below`).

## Reaching above the material

A receiver's demand can lie above its own material: the later segment's base plane rises above
the receiver's top wherever the receiver is thin or absent under it, and the column has to be
printed in the receiver's layers all the same. `CutSegmentDescriptor::support_z_max` records,
per segment, the highest point along its build axis where material on the far side of any of
its ceiling planes lands on that plane (every far-side vertex projected onto the plane, kept only
inside the segment's other bounds). `PrintObject::segment_slice_top()` extends the segment's layer
schedule up to it, plus slack for the contact placement below (only for segments that have a
ceiling; a leaf slices to its material), and `slice_volumes` keeps the resulting empty top layers
instead of stripping them. Layers of a segment with nothing to print are dropped at G-code
export, and the G-code viewer keys its layers on the source layer id changing rather than on
consecutive ids. A material layer under empty layers still counts as the object's top for
"topmost surface" ironing (`Layer::no_material_above`).

## Two kinds of load

The footprint is handed down in two kinds and kept apart on the receiver
(`segment_support_demand()` and `segment_body_demand()`). Under the child's **support** the
receiver's column is support-on-support: it ends flush with the plane. Under the child's
**body** the column is an ordinary overhang and keeps the normal top Z distance, so the model
can still be separated from it. A floating segment's own suspended first layer is body demand.

## Ending flush under the ceiling

Inherited load is support-on-support, so the receiver's column must end at the ceiling, not a
support gap below it. Each generator places the top of a column it builds for an overhang some
distance below the overhang's layer: the normal generator a contact gap under `bottom_z`, the
classic tree a virtual gap node one layer down plus `top_z_distance`, the organic tree
`z_distance_top_layers + 1` layers. Each therefore reads the demand of layer *i* from that many
layers above *i* when it seeds its overhangs, so the top of the column lands within a layer of
the plane; the segment-bounds clip (`SegmentSupportBounds`) trims whatever a tilted ceiling cuts
off. The child side is flush already: its first support layer's bottom is its floor.

Layers are whole, so under a tilted plane the receiver's column still stops up to one layer
short of it. `support_section_overlap` (per object, mm) lets the support-on-support column reach
that far past the ceiling: the contact is aimed that much higher and the bounds clip admits the
extra reach inside the column only (`SegmentSupportBounds::valid_region_polygon` with a column).
One layer height closes the seam at the cost of a little over-extrusion where the two columns
meet; it is never applied under the model.

## Bounds

`SegmentSupportBounds` re-expresses the segment's clips in its slice frame. Floors (the planes it
rests on) bound the support and are surfaces it may stem from; ceilings (planes it sits below)
only clip. The bounds compose with the support-direction shear when support leans (see
`support-direction.md`).
