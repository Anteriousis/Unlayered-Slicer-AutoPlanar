# Experimental native curved layer blend

The engine slices the original mesh through one continuous coordinate field,
generates contours and infill in that domain, and emits physical XYZ paths through
the inverse field. This is native slicing and Preview integration. It is not a
physically validated printer profile or a guarantee that every plane can be blended.

## User flow

Use **Advanced → Process → Others → Multiplanar → Curved layer blend (experimental)**.
Enable nonplanar printing, choose a Virtual printer and keep belt printing off.
Keep process layer rotations B/C at zero. Add or accept planes whose centres are
ordered by height and whose surfaces do not intersect within the model bounds.
Press Slice plate yourself and inspect Preview before exporting. The original
object is sliced as one object; planes guide layer direction without creating
separately sliced sections.

A plane specifies its target build direction and the centre height of the
transition, not a sharp material cut. The engine chooses the transition depth
from object size, direction change, layer spacing and clearance margin. Crowded
planes, steep changes and insufficient transition height are refused with a reason.
Suggest planes still estimates piecewise-planar results; its cost estimates and
section warnings do not predict curved-blend output.

The mesh may have a curved roof, curved outline, through holes and other contours.
The current integration supports one closed mesh, one object/copy/volume. Supports,
modifiers, painting, variable heights, independently directed branches and
intersecting branch planes are not integrated. Bridge extrusion is refused during
path generation. These are planning limits, not a ramp-only mesh restriction.
With no planes, a certified rectangular ramp remains a calibration mode blending
from its flat foundation into its exact roof.

Classic walls are required. Combined infill, scarf seams, ironing, fuzzy skin,
Z anti-aliasing, wave overhangs, wipe and circular arcs are off. Ordinary retraction
is available. Draft shields and adaptive pressure advance are refused. XY and
elephant-foot compensation and Z offset are zero; shrinkage compensation is 100%.
Layer-change code may be empty or G92 E0. External slicing/G-code scripts,
timelapse motion and height-change code are outside this integration. Start/end
code remain in the physical machine frame.

Project reload keeps saved planes at their world positions when a single-volume
object's local origin is normalized. The G-code height header uses the physical
deposition bound in curved mode; its logical layer coordinate is not a height
above the bed. End-code height placeholders continue to use actual deposited Z.

## Field and geometry bounds

For selected planes the physical-to-logical mapping is
`w = z - gx(z)*(x-cx) - gy(z)*(y-cy)`. A quintic smoothstep blends each successive
target gradient over a physical height interval. Intermediate constant-w layers
are curved in XY, not merely tilted flat planes. The foundation is identity-mapped.
After a blend, layers follow its target direction until the next blend. The last
target need not match an arbitrary curved roof: the original mesh still determines
where material exists.

The maximum smoothstep derivative is 15/8. Bounds on its projected displacement
over the entire model rectangle establish a strictly positive Jacobian and an
upper bound on every intermediate slope. The engine reserves clearance margin
for that bound. The general plane program conservatively uses the worst configured
azimuth; each target is also checked in its own direction. The certified ramp uses
its known directional bound. Sparse facet sampling is not a collision certificate.

The physical mesh is copied and conformingly refined using a global Hessian bound
before changing coordinates. Shared edge midpoints preserve connectivity and hole
topology. The bound limits physical boundary interpolation error to 0.01 mm before
float rounding and normal slicer simplification. Resource limits fail explicitly
rather than silently coarsening the mesh. The ramp boundary maps exactly. The
user's model and Prepare geometry are unchanged.

An integer schedule respects explicit printer minimum normal spacing and maximum
vertical layer height. Requested nominal layer step is an upper bound; the engine
may choose a smaller logical step. Paths are subdivided using a Lipschitz bound
of at most 0.005 mm vertical chord error before coordinate rounding. Physical bead
area and segment length replace the planar geometric flow factor while retaining
material/role ratios. Narrow classic gap fill uses the same positive-area
width/height convention as upstream Flow. Its speed bound accounts for the area
maximum inside the permitted height interval, not just at its upper endpoint.
Nonpositive or nonfinite mapped flow is refused. This bead model is not a bonding
guarantee.

## Motion and Preview

The writer emits physical XYZ, local bead-height and layer-normal tags. Preview
processes those same paths and orients extrusion cross-sections. Virtual mode
keeps the actual nozzle upright and emits no rotary B/C motions.

The represented asymmetric hotend pyramid and gantry bounds apply to the whole
layer field. The legacy checker for independent cut-plane episodes does not
partition a continuous curved pass correctly and is bypassed only for this bounded
mode. This does not claim a general CAD mesh collision checker or cover missing
hardware. With one mesh and no supports, earlier material lies beneath earlier
field isosurfaces, allowing the global slope bound to bound the represented head.

For general roofs, contour midpoint sampling can deposit up to a fraction of a
layer above the CAD top. The travel envelope reserves a whole maximum physical
layer plus 0.02 mm for mapping/path/rounding error; output outside that envelope
is refused. That inflated height is also checked against machine and Virtual Z
limits. This is a toolpath bound, not a measured allowance for material swelling.

Travel leaves vertically, traverses above the bounded printed paths by at least 2 mm and
descends vertically. Every such travel requests ordinary retraction, including
short nominal XY gaps. XY and Z speed are separate; Z velocity and volumetric flow
respect configured limits. This may be slow and does not establish string-free
material tuning. Model height plus travel must remain below machine and Virtual Z
limits. The no-plane ramp's 10 mm cap is a calibration scope restriction. No number
here establishes a measured generally safe nonplanar print height.

The option defaults off. Serialization, preset lists and slice invalidation are
wired into the normal app flow. Disabling it restores ordinary slicing. Windows
packaging retains stock preset JSON files and the cached preset catalog.

## Verification and remaining work

Standalone tests cover field inversion, curvature, spacing, successive direction
changes and infeasible-plane rejection. Native fff_print tests cover actual mesh
slicing, original-model preservation, curved roofs, a through hole, conforming
refinement, output, Preview, flow, travel and disabling/reslicing. Record actual
results and exact commit before distributing a build.

Intersecting branch fields, multiple object scheduling, support/contact planning,
physical bridge classification, seam optimization across changing topology and
full measured toolhead meshes need further integration and regression cases.
Software tests and physical prints are separate evidence.
