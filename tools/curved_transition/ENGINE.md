# Experimental native curved layer blend

The engine slices the original mesh through one continuous coordinate field,
generates contours and infill in that domain, and emits physical XYZ paths through
the inverse field. This is native slicing and Preview integration. It is not a
physically validated printer profile or a guarantee that every plane can be blended.

## User flow

**Automatic layer mode (experimental)** runs inside Slice plate.
It takes precedence over **Manual curved layers (experimental)**. The manual
control is disabled and labeled inactive while automatic mode is on, including
when an older preset has both saved switches on. Switching automatic mode off
restores access to the saved manual choice; the UI does not rewrite either value.
Existing enabled planes
are tested as a continuous stack. Otherwise the selector tries a bounded global
roof direction and local surface-region proposals, including two successive
directions. Up to eight proposals run through the actual field, conforming mesh
mapping, layer schedule and configured printer-limit checks. The original model
and its saved plane list are preserved; inferred planes are internal to the slice.

The roof direction and regional planner scores are mesh-based finish heuristics.
The regional scores describe piecewise-planar estimates and only rank proposals;
they do not predict curved print time, support savings or surface quality. The
selector does not yet optimize strength or independently directed branches.
Symmetric curved surfaces need not have a beneficial global direction. Supports,
unsupported undersides, less than 100% infill, incompatible process options and
infeasible transitions keep the job planar without changing those settings or
raising any printer limit. Explicit B/C rotations or directed parts conflict with
automatic selection and require the user to choose manual mode instead.

The decision and reason are displayed as a slicing notification and recorded in
the `layer_strategy` G-code header. A later extrusion or motion safety refusal
still stops slicing: automatic mode does not conceal such errors or export a
partially failed curved job. It is off in existing presets by default. An optional
automatic trial process is provided alongside the explicit curved trial.

For manual mode, turn automatic mode off and use
**Advanced → Process → Others → Multiplanar → Manual curved layers (experimental)**.
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

The full MCT-1 totem is a broad geometry and piecewise-planar planner benchmark,
not a supported whole-object curved calibration model. Its shelves and arch have
downward surfaces that need bridge or support planning. Automatic curved mode
keeps such a job planar; forcing manual curved mode does not supply that planning.
An STL contains no authored cut planes. **Suggest planes** proposes saved
piecewise-planar cuts, whereas automatic curved mode's candidates remain internal
to the slice. Check the slicing decision before using a completed totem print as
evidence that any curved layers were printed.

Classic walls are required. Combined infill, scarf seams, ironing, fuzzy skin,
Z anti-aliasing, wave overhangs, wipe and circular arcs are off. Ordinary retraction
is available. Draft shields and adaptive pressure advance are refused. XY and
elephant-foot compensation and Z offset are zero; shrinkage compensation is 100%.
Layer-change code may contain the checked cooling/progress commands or G92 E0. External slicing/G-code scripts,
timelapse motion and height-change code are outside this integration. Start/end
code remain in the physical machine frame.

## Fixed-nozzle dual-carriage mode

The dual-carriage integration uses the existing BBL filament/nozzle protocol,
with one used filament manually mapped to one nozzle throughout deposition.
The physical printer profile remains two-nozzle. Independent carriages, nozzle
clusters, toolchangers, automatic reassignment and in-print tool/material changes
are excluded. This mode is intended for fixed-nozzle X2D commissioning; it does
not establish its physical clearances or support simultaneous printing.

Printer settings provide a fixed active nozzle number (1 or 2), the active nozzle
used as the clearance origin, a whole-toolhead-recorded declaration and a source.
The existing common envelope/angles, gantry and Virtual Z bounds then describe
the whole head in that one state, including the parked nozzle. Both nozzle
identities must match. Changing the active nozzle requires reviewing these common
bounds and recording their new origin. No X2D geometry values are supplied.

The engine checks the active printable footprint/height, layer range, finite
offset and normal-mode Z speed. The writer pins the planned nozzle and refuses
tool commands during curved deposition before altering its state. Startup still
uses the existing stock physical-tool mapping and BBL protocol. Fixed mode
resolves X2D's initial hotend placeholder explicitly and checks the expanded
startup's last ordinary T command selects the planned filament/nozzle with H;
automatic H-1 selection is refused. This is a command check, not observation of
the firmware's physical response. Raw start/end
and filament scripts, firmware compensation and mechanical startup actions need
separate review; the field does not certify those actions.

Expanded layer scripts may contain cooling/progress commands M106, M142, M73,
M991 and G92 E0. Axis motion, coordinate resets, tool changes and other commands
are refused before emission. This preserves stock Bambu layer cooling/progress
without permitting raw XYZ movement through the curved layer path. Imported
Preview maps physical-nozzle offsets to filament slots once and retains the
already zero-based identity of a job using only nozzle 2.

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

For fixed dual-carriage jobs, expanded startup selection tracks both BBL T/H
and M1020 S/H commands. Parking/unloading or an unsupported selection invalidates
an earlier selection; out-of-range IDs are rejected before numeric conversion.
The final explicit selection must match the planned used filament and nozzle.
This checks emitted commands, not the printer's actual firmware state.

Expanded filament-start, layer and extrusion-role scripts in a curved job may
only control the permitted cooling/progress commands or emit G92 E0. Raw axis,
frame/tool commands and silent changes to position/extrusion writer-state output
variables stop export before the writer accepts those changes. Stock start/end
and calibration macros still need independent physical/firmware review.

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
