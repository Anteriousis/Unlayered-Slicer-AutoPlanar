# Changelog

All notable changes to Unlayered Slicer are listed here, newest first.

## 0.1.3 — 2026-10-11

Changes since **0.1.2** (2026-10-10).

### New

- **Anonymous usage diagnostics.** Unlayered Slicer now tells us which features are used, how
  slicing went (including errors), when it crashes, and basic system details (operating system,
  processor cores, memory, language), with a random id that is not linked to you or your account.
  Models, G-code, file and object names, and the names of your own presets are never sent. It is on
  by default: turn it off in **Preferences > Online > Send anonymous usage diagnostics**, or set the
  `UNLAYERED_NO_TELEMETRY` environment variable to switch it off for the whole computer. A notice
  says so the first time it runs.
- **macOS: the slicer updates itself**, as it already did on Windows. Signed in with your Unlayered
  account, a new version downloads in the background and replaces the app when you choose *Restart
  to update* or close it. This works when the app is in *Applications* (or *Applications* in your
  home folder); a copy anywhere else still opens the download page.

### Changed

- **Easier to get started with multiplanar printing.**
  - The Cut Planes tool is greyed out until non-planar printing is turned on for your printer. Its
    tooltip now says so. Clicking it offers to turn non-planar printing on, with Printer type set
    to Virtual (for an ordinary 3-axis printer). Unlayered 5-axis printers already have it on.
  - *Enable non-planar printing* and *Printer type* (Printer settings > Motion ability) now show in
    Simple mode. Before, you had to switch to Advanced mode to find them.
  - The multiplanar tutorial (Help > Multiplanar Tutorial) has been rewritten. It explains what a
    cut plane is for, lists the settings you need *before* the quick start, shows the toolbar
    icon, gives the correct setting locations, and adds a settings reference.
  - The help links on the non-planar and layer-rotation settings open that tutorial. Before, they
    went to web pages that don't exist.

### Fixed

- **macOS: "Unlayered Slicer quit unexpectedly" on every quit** while the Bambu Lab network plug-in
  was installed. The plug-in is now unloaded before the app exits.
- **Linux: embedded web tabs (such as Universe) could open blank.**
- **Building from source on macOS and Linux** works again, including the universal (Intel and Apple
  silicon) macOS build.

## 0.1.2 — 2026-10-10

Changes since **0.1.1** (2026-09-21).

### ⚠️ Safety fixes — please update

- **The nozzle could drive down into a finished multiplanar print.** On a print with cut planes
  (multiplanar or virtual 5-axis, printed *By group*), the `{max_layer_z}` value given to the
  printer's start/end G-code was the height of the *first* chunk, not the top of the part. Bambu
  Lab profiles end with `G1 Z{max_layer_z + 0.5} ; lower z a little`, so at the end of such a print
  the nozzle moved straight down into the part — seen on a Bambu Lab H2S, where it went about 30 mm
  into a finished Benchy. `{max_layer_z}` now always reports the true top of the printed part.
  Ordinary flat prints are unaffected. **If you print multiplanar or virtual 5-axis jobs on a Bambu
  Lab printer, do not use 0.1.1 for them.**
- **Virtual 5-axis: steep tilts are refused when the hotend would hit the part.** The tilt limit
  derived from your hotend's shape used to apply only when a gantry height was entered; it now
  applies whenever the hotend envelope is on. With the default envelope, tilts beyond roughly
  35–45° are refused with a message naming the hotend side that limits them. *Settings that used to
  slice may now be refused; that is intended.*
- **The hotend is now modelled as a four-sided pyramid** (separate angles for the +X, −X, +Y and −Y
  sides) instead of a single cone, and the virtual 5-axis collision check uses the head's real,
  upright orientation. Collision checks also follow G2/G3 arcs.
- **Bambu Lab P1S:** printing no longer clears the printer's SD-card state.

### Fixed

- **Supports rotated 90° away from the part** when multiplanar printing was on and *Layer rotation
  B* or *C* was changed. Supports now print under the part and stand on the plate.
- **macOS: "Slice Plate" crashed** on colour-painted objects whose project had fewer filament
  colours than filaments.
- **macOS and Linux builds** compile again.
- **Bambu Lab network plug-in could not be installed** ("network plug-in not found" with no way to
  install it). It can be installed again — from the installer, the first-run setup, or
  Preferences › Online — and is loaded correctly from long installation paths.
- Cut-plane pieces that sit off the build plate no longer get mixed into the shared brim.
- Auto drop (placing objects on the bed) is on by default again.
- Projects with missing or partial settings, and meshes with invalid vertices, no longer crash the
  loader or the command line.
- Painted facets (supports, seams, colours) land in the right place on rotated and cut objects.
- Copies of rotated objects each get their own orientation.
- 5-axis G-code from other software previews correctly.
- "Bind with Pin Code" explains why it cannot continue instead of silently doing nothing.

### New

**Getting started**
- **New first-run setup page — "Privacy and printer connections".** One page asks whether to
  enable Stealth Mode (off by default) and whether to install the Bambu Lab network plug-in (on by
  default), which is required for wireless communication with Bambu Lab printers.
- **The slicer updates itself.** Sign in once with your Unlayered account and new versions download
  in the background; choose *Restart to update* (or just close the app) to install. Help menu:
  *Restart to Update*, *Sign in*, *Sign out*. An interrupted download picks up where it stopped.
  (Updating *from* 0.1.1 still means installing 0.1.2 by hand once; from 0.1.2 on it is automatic.)
- **"What's new" after an update.** The first time a new version starts, it shows the changes since
  the version you had before, taken from this changelog.
- New installations start in the Dark colour scheme.

**Appearance and accessibility**
- **Colour themes:** named themes you can create, edit live, save, rename, import and export, with
  bundled *High contrast* and *Neon* presets. Every part of the window follows the theme — toolbars,
  3D view background, build plate, grid, sliders, notifications and overlays.
- **Colour-vision previews** to check a theme for colour blindness, and a contrast audit.
- **Font choice and font scale**, including OpenDyslexic and Comic Sans, applied without restarting;
  dialogs and controls scale with the font.
- **Full keyboard navigation:** a visible focus ring, Tab through the main tabs, menus, settings
  pages, sidebar, drop-downs and 3D-view toolbars in on-screen order; Space/Enter press buttons.
- Preferences is resizable, remembers its size, and applies theme changes live.

**Printing**
- **Wave overhangs** are no longer experimental. *(Ported from Dennis Klappe's
  [OrcaSlicer-WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs).)*
- **Mesh infill:** Catfill, Puppyfill, custom SVG patterns (previewed on the plate) and infill-mesh
  volumes, with orientation control and a single-wall option.
- **Belt printers:** slicing along a tilted gantry, support resting on the belt, brims, purge tower,
  several objects interleaved along the belt, and cut planes on a belt. New profiles: **Printcepts
  BabyBelt Pro** and **IdeaFormer IR3 V2**.
- **Contact-aware first layer:** first-layer speed, flow and fan now follow each line's height above
  what it rests on, rather than its layer number. On belt printers, every strip lying on the belt
  gets first-layer treatment; a cut-plane piece resting on plastic no longer gets bed-style first
  layers.
- **Support direction:** lean support along a chosen world direction; cut-plane supports carry the
  load of every piece above them and stop flush under the material they hold up.
- **Bed mesh overlay:** see your printer's probed bed mesh on the plate (RepRapFirmware and
  OctoPrint), with an out-of-flat warning.

**Unlayered printers**
- The printer configuration updater refuses to install files onto firmware too old to run them, so
  an update can no longer leave a printer that stops part-way through its startup file.

### Changed

- **Bambu Lab cloud features have been removed.** Cloud sign-in, cloud printing, the privacy-policy
  dialog and pin-code account binding are gone. Bambu Lab printers are used over the local network
  (LAN mode, with an access code); the network plug-in is still needed for that.

### Known issues

- **Windows may warn about the installer** ("Windows protected your PC", or an antivirus alert).
  The installer is not code-signed yet. Choose *More info › Run anyway*. If a download says
  *"installer integrity check has failed"*, the file was incomplete — download it again.
- The paint-on supports preview does not yet follow the rotation of a rotated or cut object; the
  sliced supports are correct.
