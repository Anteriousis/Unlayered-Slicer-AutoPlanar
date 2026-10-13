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
Symmetric curved surfaces, unsupported undersides, supports and sparse infill use
independent compact roof fields instead of requiring a single global direction.
Downward-face contacts have protected planar zones. Native bridge/support bead
boxes are inspected after path generation; intersecting local patches are removed
and the original solid is re-sliced, up to 64 bounded retries. An object with no
remaining useful region stays planar. No process setting or printer limit is
changed. Explicit B/C rotations or directed parts conflict with
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
Each object requires one closed mesh volume. Multiple different models and copies
can share a plate and have independent fields, including planar objects. Modifiers,
painting, variable heights and intersecting authored branch planes remain outside
this integration. Automatic mode preserves planar bridges and support/contact
regions and uses native support generation; it does not make curved unsupported
bridges printable or remove support solely because a roof can curve.
With no planes, a certified rectangular ramp remains a calibration mode blending
from its flat foundation into its exact roof.

The full MCT-1 totem is a broad geometry benchmark. Its shelves and arch retain
planar bridge/support planning while eligible roof regions can use curved layers.
Check the actual slicing decision and Preview: finite patch budgets, competing
contact zones and process/clearance checks can leave features or the whole object
planar. Manual global plane blending still has no general support planner.
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

Regional mode uses `w=z-S(z)*g.(x-c)*phi(|x-c|/r)` with
`phi(t)=(1-t)^4*(1+4t)` inside disjoint disks and zero outside. Both the spatial
taper and temporal blend are C2. The spatial gradient operator has eigenvalues
`phi` in [0,1] and `phi+t*phi'` in [-7/16,1]; displacement is bounded by
`0.17*|g|*r`. Temporal derivative and Hessian bounds establish monotonicity, slope,
normal spacing, conforming mesh error and emitted path subdivision. Local Hessian
boxes avoid refining identity facets unnecessarily, with shared edge splits still
propagated to neighbours. The selector tries several patch radii and uses equal-area
barycentric quadrature over affected upward facets, including adjacent flat roofs.
It requires at least a 2% sampled reduction in projected squared roof slope in the
logical slice frame. This rejects compact patches that merely move stair-stepping
to their taper. The proposal count is bounded at 256 and the accepted patch count
at 64; cancellation is checked during selection. The score is a roof-alignment
heuristic, not a surface finish or support-saving measurement. A patch tapers to horizontal at its edges;
it need not flatten an entire curved roof.

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
hardware. Within one object, earlier material lies beneath earlier field
isosurfaces; supports and bridge beads must remain entirely in identity zones.
Different objects additionally receive whole-footprint checks including generated
supports and every instance. The generated first-layer hull, including brim and
skirt bead margins, is checked against the common and active-nozzle printable areas.
By-layer printing (or one shared nonzero print group)
uses each field's displacement bound plus the larger actual layer allowance to bound relative
height. Sequential groups use full part heights and also include generated brim
and skirt paths in the nozzle approach footprint against neighbouring models.
Insufficient head/gantry spacing
is refused with a message; the engine does not silently raise clearance limits.
All members of a curved plate must use compatible frame/process settings. Protected
planar paths retain ordinary overhang speed and fan splitting; curved paths use the
physical subdivision and flow branch.

For general roofs, contour midpoint sampling can deposit up to a fraction of a
layer above the CAD top. The travel envelope reserves a whole maximum physical
layer plus 0.02 mm for mapping/path/rounding error; output outside that envelope
is refused. That inflated height is also checked against machine and Virtual Z
limits. This is a toolpath bound, not a measured allowance for material swelling.

Travel leaves vertically, traverses above the whole plate's bounded printed paths by at least 2 mm and
descends vertically. Its height uses the greatest physical deposition Z emitted
so far, plus a bound on tilted bead-edge rise and the configured clearance
(at least 2 mm). The same bound survives changes between objects, instances,
planar support paths and curved fields; future unprinted layers do not raise it.
Unknown positions after custom code retain the full final-plate height fallback.
Custom machine-start routines must finish with a prepared bed and may not leave
unmodeled obstructions; firmware macros and material swelling are not inferred.
An XY detour retains all planned waypoints at one raised height, with one
departure and one final approach. Exact repeated destinations emit no motion.
These changes do not reduce the configured clearance or raise motion limits.
Every actual travel requests ordinary retraction, including
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

Intersecting authored branch fields, general curved bridge deposition, seam
optimization across changing topology and full measured toolhead meshes need
further integration. Supports currently stay planar; tree paths crossing a local
field remove that patch during automatic planning. Rafts remain excluded. The
selector does not promise that every model has a useful curved region, nor that
its limited local adjustment improves the printed finish.
Software tests and physical prints are separate evidence.
