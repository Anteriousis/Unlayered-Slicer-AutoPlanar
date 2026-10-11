# Experimental native curved layer blend

This branch connects the transition field to the actual mesh slicer and native
G-code writer. It is a deliberately narrow first integration, not general curved
surface slicing and not a physically validated printer profile.

## User flow

Use **Advanced → Process → Others → Multiplanar → Curved layer blend (experimental)**.
Enable nonplanar printing, choose a Virtual printer and keep belt printing off.
Import a single shallow rectangular ramp, leave layer rotations B/C at zero and
remove cut planes. The user presses Slice plate, reviews Preview, exports and
starts a print. Suggest planes remains the separate piecewise-planar feature.

This first version accepts one object/copy/volume, a closed mesh with a flat bed
face, four vertical rectangular sides and one planar shallow roof. The curved
transition spans from the flat first layer up to that roof. A genuinely curved
roof, supports, branches, cavities, modifier volumes, painting, cut-plane stacks,
variable heights, multiple objects and belt/rotary printers are refused.

The process must have 100% infill, classic walls, no combined infill, scarf seam,
ironing, fuzzy skin, Z anti-aliasing, wave overhangs, wipe or circular arcs. Normal
retraction is available. XY shape compensation and Z offset must be zero;
shrinkage compensation must be 100%. Layer-change code may be empty or G92 E0.
External slicing/G-code scripts, timelapse motion and height-change custom G-code
are outside this integration. Start and end code stay in their physical frame.

## What the engine does

* Verifies all triangles lie on the six oriented boundary faces, their areas
  cover exactly one ramp, and the mesh is closed. Checks instance and volume
  transforms before certification; does not mutate the user's model.
* Flattens a private copy of the actual solid before slicing. For this domain
  the entire boundary maps exactly to a cuboid: no curved facet approximation is
  required. Standard contour, perimeter and solid infill generation then operate
  in that flat coordinate domain.
* Chooses an integer layer count and reference height that land the final layer
  on the original roof and satisfy the printer's explicit minimum normal gap and
  maximum vertical layer height. A profile's nominal step is an upper bound;
  the generated logical step can be smaller. No setting is silently rewritten.
* Maps each path back through the transition field and subdivides it. A global
  Lipschitz bound certifies at most 0.005 mm vertical chord error before G-code
  coordinate rounding. Physical bead area and physical segment length replace
  the original geometric flow factor; material/role flow ratios are retained.
  The bead model is an engineering approximation, not a bonding guarantee.
* Writes physical XYZ and local HEIGHT tags natively. Preview processes those
  same paths before the user exports; this is not export-only Z post-processing.
* Uses the configured asymmetric hotend pyramid, the gantry limit and the manual
  angle ceiling in the actual uphill direction, including diagonals. This checks
  the represented envelope only, not missing CAD parts or physical deviations.
* Travels vertically away, traverses above the entire certified solid plus at
  least 2 mm and approaches vertically. This intentionally prioritizes a simple
  bounded commissioning case over travel efficiency. Z velocity and volumetric
  flow are limited using the configured machine/material values.
* Limits parts to 10 mm in this first integration and checks travel against the
  configured machine and virtual Z ceilings. **10 mm is an implementation scope
  restriction, not a measured safe nonplanar print height.**

The feature defaults off. Config/preset serialization and relevant slice
invalidation are wired into the normal application flow. Windows packaging keeps
the stock JSON preset catalog as well as the preset cache, so loading a portable
build does not lose its stock printer parents.

## Verification

The dependency-free geometry tests run through the standalone CMake project in
this folder. The main `fff_print_tests` additionally exercises real model
slicing, native output, writer subdivision/flow/travel, rejection behavior,
configuration round trips and re-slicing after disabling the feature. The branch
has a Windows x64 build/test workflow. Record actual results and exact commit
before distributing a build; a compiled application is not a physical print
validation.

## Further work

Support arbitrary verified height-field roofs with mesh refinement and domain
handling, physical bridge/contact classification, solid/sparse infill testing,
additional model topology, supports, multiple objects, physical seam planning,
and a measured full toolhead model. Those require independent integration and
tests. This version must not silently claim they are implemented.
