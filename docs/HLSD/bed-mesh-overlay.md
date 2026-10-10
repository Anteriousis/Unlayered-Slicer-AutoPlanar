# Bed mesh overlay

Shows the bed mesh the printer's firmware currently compensates with as a height map on the
plate, so a user can see where the bed is high or low under the parts they are placing.

## What it does

`View > Show Bed Mesh` (also in the in-canvas view popup) reads the mesh from the current printer
and draws it on every plate at that plate's origin, in printer coordinates. `View > Reload Bed
Mesh from Printer` reads it again after re-probing. The mesh is not saved with the project; it
describes the printer at the time it was read and is dropped when a printer preset with a
different host is selected.

The plate shows three things:

- A colour-coded surface over the probed rectangle. Colour encodes Z through a diverging ramp:
  cool below the plane, neutral grey on it, warm above it. The range is symmetric about zero and
  set by the largest excursion, with a floor of 0.005 mm so a flat bed does not get stretched
  into false relief.
- The outline of the probed rectangle.
- A small mark at each probe point.

An ImGui legend under the main toolbar shows the ramp with its extremes, the profile name, the
probe grid, the Z extremes and their peak-to-peak range, the data source, and how much of the
printable area the mesh covers. A bed more than 0.2 mm out of flat peak to peak gets a worded
warning in the legend and a warning notification on fetch: compensation hides that much, but
it points at tramming or a warped plate rather than something to live with. When the toggle is
on but nothing has been read yet, the same spot says so and points at the reload command;
nothing is fetched at startup on its own.

## Partial (adaptive) meshes

Klipper's adaptive meshing and KAMP probe only the area the next print occupies. Moonraker's
`bed_mesh` object reports that region as `mesh_min`/`mesh_max`, and both `probed_matrix` and
`mesh_matrix` cover only it. The firmware clamps to the edge value outside the region, which is
not knowledge of the bed, so the overlay draws nothing there. The legend labels a mesh partial
when it spans less than 70% of the bed on either axis: a full mesh loses only the probe offset
at the edges, whereas an adaptive one typically covers a fraction of the bed.

## Not confirmed as loaded

A mesh can exist on the printer without being the one it compensates with. Klipper drops the
active mesh whenever it restarts, including the restart `SAVE_CONFIG` triggers right after a
calibration is saved, and only `BED_MESH_PROFILE LOAD=<name>` brings it back; RepRapFirmware
keeps `heightmap.csv` on the SD card but only uses it after `G29 S1`. In those cases the parser
still returns the mesh, flagged `loaded = false`, and the legend and the fetch notification say
so, since a mesh the printer is not compensating with should not be mistaken for one it is.

## Data path

`BedMesh` (libslic3r) is the firmware-independent model: profile name, source, the rectangle,
the raw probed grid and the firmware's interpolated grid, rows along Y and columns along X. A
node the firmware did not probe (the corners of a delta's rectangle outside its radius, a UBL
cell printed as `.`) is NaN: the statistics, the flatness warning, the surface cells and the
probe marks all skip it, and the legend counts probed nodes out of the grid. RRF nodes sit at
`min + i * spacing`, which is what the firmware probes; the header's `max` need not be a
multiple of the spacing and is not used for placement. `BedMesh` holds the parsers for every
format the hosts return and is unit-tested without the GUI.

`PrintHost::get_bed_mesh` is the per-host-type readback:

- `Moonraker`: `GET /printer/objects/query?bed_mesh`. With no active mesh it falls back to the
  saved profile `default`, else the first profile.
- `Duet` (and therefore `Unlayered`): asks the object model which map is loaded
  (`rr_model?key=move.compensation` standalone, `machine/status` under DSF; `file` is null
  when none is), then downloads it with `rr_download` or `machine/file`, defaulting to
  `0:/sys/heightmap.csv`. RRF 2 has no object model, so the default path is assumed.
- `OctoPrint`: Marlin prints its mesh only on the serial console, and OctoPrint exposes the
  console only on its push socket. The socket is SockJS, which also speaks plain HTTP polling,
  so the readback needs no browser and no websocket: passive login with the API key
  (`POST api/login`), open a polling session (`POST sockjs/<srv>/<sid>/xhr`), authenticate on
  it, check `api/printer` is operational and idle, send `@BEDLEVELVISUALIZER` and `M420 V`
  through `api/printer/command`, then poll and parse `current.logs`. If the Bed Level
  Visualizer plugin is installed, its `plugin` message with `mesh` and `bed` arrives on the
  same socket and is preferred because it carries bed bounds; Marlin's bilinear grid comes
  without positions and is stretched over the printer profile's bed, which the note says.
  UBL's topography report includes corner coordinates and those are used. As a last resort
  the copy the plugin's web UI may have stored in `api/settings` is used, flagged not loaded.
  `PrusaLink` opts out: it is not OctoPrint and has no console access.

A printer whose host type is OctoPrint is queried as Moonraker first, because Klipper users
commonly keep that type for Moonraker's OctoPrint emulation and the query fails fast on a real
OctoPrint; then as OctoPrint. Other hosts return "not supported".

Every step of the OctoPrint and Duet readbacks logs under `OctoPrint: bed mesh:` and
`Duet: bed mesh:` (info level, bodies at debug) so a failure on an unfamiliar printer can be
read off `<datadir>/log/debug_*.log`.

`Plater::fetch_bed_mesh` runs the blocking requests on the UI job worker, trying the hosts in
order, and hands the result to `BedMeshOverlay`, which `PartPlateList` owns because the mesh
belongs to the printer, not to a plate. `PartPlate::render` draws it just above the plate
decorations with the depth mask off, so printed objects at z >= 0 still occlude it.

## Rendering

`GLModel` has no per-vertex colour layout, so the surface is a `P3T2` grid whose texture
coordinate is the normalised Z, sampled from a 256 x 1 ramp texture through the `flat_texture`
shader. Interpolating the texture coordinate between nodes interpolates the height rather than
the colour, so the neutral band stays where Z crosses zero. The ramp texture is regenerated when
the palette changes; the models when the mesh or the plate height changes.

## Colour vision

The ramp follows the `preview_color_vision` preference the G-code preview already uses, so both
switch together:

| Mode | Low pole | High pole |
|---|---|---|
| Normal | blue `#0571b0` | red `#ca0020` |
| Protanopia, deuteranopia | blue `#0072b2` | orange `#e69f00` |
| Tritanopia | green `#009e73` | vermilion `#d55e00` |

Each pair was checked for pole-to-pole separation under simulated protan, deutan and tritan
vision (OKLab ΔE ≥ 11 for the worst case, most above 20). The neutral midpoint is the same grey
in every mode, and the legend states the numbers so the colour never carries the information
alone.
