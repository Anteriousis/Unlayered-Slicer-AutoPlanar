# Mesh and SVG infill

Sparse infill from geometry the user supplies, in two forms: a triangle mesh tiled through the
part (**Custom (infill mesh)**) and SVG line art tiled per layer (**Catfill**, **Custom (SVG)**).
Both are ordinary sparse infill patterns: they clip to the same per-region fill areas as every
other pattern and coexist with modifiers, multi-material and combine-infill. This document
covers the design; the user-facing guide lives in the wiki.

## Custom (infill mesh)

### What it does

An **Infill Mesh** volume (`ModelVolumeType::INFILL_MESH`, `Add Infill Mesh` in the object menu,
serialized as `infill_mesh` in the 3MF) is the unit cell of a lattice. It contributes no sliced
body of its own; it is consumed only when a region's sparse pattern is Custom (infill mesh). A
cell is repeated on the pitch of its own axis-aligned bounding box in every direction that has a
non-zero extent, so a rectangular prism tiles as a rectangular lattice and a flat cell is not
stacked. The cell's local axes are its tiling axes, so the volume's own rotation and scale, set
with the ordinary gizmos, orient and size the whole lattice.

The lattice is built as one mesh, sliced once at every layer's `print_z`, and stored on the
`PrintObject` in `PreSliceInfillStructures::mesh_infill`. Per layer, `FillMeshBackedT` clips the
stored cross-sections to the region's internal fill area. A closed cell (a lattice of struts) is
sliced to polygons and each ribbon is walled with Arachne, two walls or one with
`sparse_infill_single_wall`, so the result follows the cell's cross-section rather than tracing
one outline. An open cell (sheets or ribbons without thickness, detected by boundary edges) has no
interior to wall: it is sliced facet by facet into segments that are chained into polylines
(`slice_sheet_mesh`), and those cross-sections print directly as single lines.

Without an Infill Mesh volume, `infill_mesh_path` names a mesh file (STL, OBJ, 3MF) used as the
cell instead: it is loaded once per path and modification time, and sits in its own frame at the
object's origin, so the volume route is the one that lets the user place and rotate the cell. Both
file settings have a Browse button on the Strength page.

### Frames and the global-orientation option

A `PrintObject` slices in its own frame: the object's local coordinates go through
`object_to_slice()`, which includes the instance transform, XY centering and, for a non-planar
print, the layer rotation that puts the object's or the cut-plane segment's build direction on
+Z. The lattice must land in that frame to line up with the layer slices.

`infill_mesh_global_orientation` chooses the lattice frame:

- **on (global)** — the lattice is mapped with `object_to_slice()`, i.e. it is fixed to the
  object. Every cut-plane segment of a multi-planar print samples the same lattice, so it runs
  continuously across the cut planes and is oblique to the layers of a tilted segment.
- **off (local, default)** — the layer rotation is dropped from that transform (the linear part
  is the instance transform alone). The lattice's Z then follows each segment's layer normal,
  the way the analytic patterns behave, and stops at the cut planes.

For an object sliced without a layer rotation the two are identical. The covered region is the
object's XY footprint over this `PrintObject`'s own layer range, which for a segment is the
segment's range, rebased to zero by `update_slice_frame()`; that box is mapped back into the
cell frame to decide which tiles are needed (`Fill/InfillMeshTiling`), so coverage holds for any
orientation without tiling the whole object.

### Invalidation and caching

`Print::apply` treats Infill Mesh volumes like support blockers and enforcers: changing, moving or
re-typing one re-runs infill preparation (`posPrepareInfill`) without re-slicing, and the volume
list is synchronized into the Print's private model copy the same way. Within a slice,
`prepare_mesh_infill_data` keeps a session cache keyed by the volume identities, their placement
in the slice frame, the orientation mode, spacing, density and the layer heights, so a re-slice
that does not touch any of those reuses the stored cross-sections.

`View > Preview Mesh Infill` builds the same lattice over the object's bounding box and attaches
it as a modifier volume named "Infill Preview", in object-local coordinates so it follows the
object; it is a visual aid and is removed like any other volume.

### Experimental: mesh-backed TPMS

The same pipeline can generate the Gyroid, TPMS-D and Fischer-Koch S surfaces as meshes from their
implicit fields (`Fill/Implicits.hpp`, `Fill/MeshGenerator.cpp`, streamed in Z slabs through
OpenVDB) instead of evaluating them per layer. It is opt-in through `UNLAYERED_MESH_INFILL=1` and
its tuning knobs (`UNLAYERED_MESH_INFILL_MODE`, `_WALLS`, `_VOXEL`, `_ADAPTIVITY`, `_SLAB`,
`_DEBUG_STL`, `_KEEP_MESH`) are environment variables, because the analytic patterns remain the
production path; what the route buys is the ability to transform infill and model together for
non-planar slicing, which the analytic per-layer patterns cannot do. The global-orientation
option applies to it in the same way (the field is evaluated in the lattice frame).

## Catfill, Sharkfill, Puppyfill and Custom (SVG)

`FillSvg` turns an SVG into a unit tile of line art: the outline of every filled shape (after
resolving its fill rule, so a silhouette with holes is one region) and the centerline of every
stroked path, flattened at 0.05 mm. Stroke width, colour and fill are followed, not rendered, so
"line art" is literal: each line is printed once at the infill line width. The tile is the SVG's
document box (`width`/`height`, resolved to mm at 96 dpi for pixel documents), which lets the
artist control the spacing between repeats; a document without a size tiles on its content
bounds. A document whose art is a `<pattern>` definition painted onto a background is read as
that pattern's cell (nanosvg does not render patterns), `<use>` clones are expanded into the
elements they reference before parsing (nanosvg does not implement them either), and a filled
shape spanning the whole document is taken for a background and skipped. The art is clipped to
the cell box, as a renderer clips a pattern to its cell, so strokes authored to run past the edge
are printed once, by the tile they land in; after the per-layer clip, fragments whose ends meet
are joined back into one line, which is what lets a tessellation designed as a continuous stroke
print as a continuous extrusion across cells.

Catfill, Sharkfill and Puppyfill are the bundled `resources/infill/*.svg`, stroke tessellations
authored as `<pattern>` cells or as `<use>` clones; Custom (SVG) reads the file named by
`sparse_infill_svg_path`. The four share one pattern icon (`resources/images/param_<value>.svg`). While the Strength page
is the active process page and an SVG pattern is selected, the art is tiled over the current
plate at print scale as a line overlay in the 3D view (`Plater::set_svg_infill_preview`),
following the infill direction and the tile scale that will print, so scale and single-wall
edits can be judged against the parts on the plate. Parsed tiles are cached process-wide by path and modification time.

Per layer the tile is stamped on a rectangular grid aligned to the absolute slice-frame grid,
like Honeycomb, so it is identical on every layer, and the layer-to-layer 90-degree alternation
is disabled so the drawing stacks instead of flipping. `infill_direction` rotates the drawing;
0 prints it upright. Lines are clipped to the fill region and chained through the standard infill
connector.

`sparse_infill_single_wall` changes what a filled shape means: bands narrower than six line widths
at the print scale (a stroked drawing exported as a fill) print along their
medial axis instead of both edges, while thicker regions keep their outline. For Custom (infill
mesh) the same option gives each cross-section one Arachne wall instead of two.

The printed size follows `sparse_infill_svg_scale`. At the default of 0 the tile scale is derived
from the sparse infill density so that line length times line width over tile area equals the
density, the same contract as every other pattern; any other value prints the SVG at its native
millimetre size times that percentage and the density setting no longer affects it.
