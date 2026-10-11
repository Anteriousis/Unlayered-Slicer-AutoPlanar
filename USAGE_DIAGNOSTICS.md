# Usage diagnostics

Unlayered Slicer sends anonymous usage diagnostics to Unlayered 3D. They tell us which features are
used, which printers and materials people slice for, and what fails, so we can fix the right
problems. This page lists **everything** that is sent. The code that collects it is
[`src/slic3r/Utils/UsageTelemetry.cpp`](src/slic3r/Utils/UsageTelemetry.cpp) and the call sites
listed at the end. The [Unlayered 3D privacy policy](https://unlayered3d.com/legal/privacy-policy)
(section I) covers the same data.

## Turning it off

Diagnostics are **on by default**. A notice says so the first time the app runs. To switch them off:

- **Preferences ▸ Online ▸ Send anonymous usage diagnostics**. Switching it off also deletes
  everything queued on the computer and not yet sent.
- **For a whole computer**, set the environment variable `UNLAYERED_NO_TELEMETRY` to any value.
  Nothing is collected or sent while it is set, and the Preferences checkbox says why it is locked.
  This is for organizations that deploy the slicer centrally.

Diagnostics are separate from stealth mode. Stealth mode does not switch them off.

## What is never sent

- Models, meshes or any geometry
- G-code
- File names, folder names or paths
- Object, part, plate or project names
- The names you give your own presets: they are reported as the built-in preset they are based
  on, or as `custom`
- Values you typed into settings (only the *names* of changed settings are sent, see below)
- Your Unlayered account, email address or name, and anything else that identifies you
- Printer addresses, access codes, serial numbers or other printer identifiers

## Identifiers

Every batch carries:

| Field | What it is |
|---|---|
| `install` | A random id (a UUID) created on your computer when the app first starts, stored in the app's settings. It is not derived from your hardware or account, and is not linked to either. It is never sent while diagnostics are off. |
| `session` | A random id created each time the app starts. |
| `app.version` | The Unlayered Slicer version, for example `0.1.3`. |
| `app.channel` | `release` or `dev` (a development build), so developers' copies can be told apart. |
| `app.os` | `windows`, `mac` or `linux`. |

## Events

Each event has a name, a timestamp and the properties below. Nothing else is attached.

| Event | When | Properties |
|---|---|---|
| `app_start` | Once per launch, about 30 seconds in | Operating system version (for example `10.0.26100`); `x64` or `arm64`; the app's display language; number of processor cores; installed memory in GB (rounded); whether this is the first launch of a new installation; whether stealth mode and dark mode are on; whether the Bambu Lab network plug-in is off, loaded or failed to load, and its version when loaded. |
| `preset_selected` | When a different printer, process or filament preset is selected | Which kind (printer, process or filament); the built-in preset's name, or the built-in preset a custom one is based on, or `custom`; whether it is a built-in preset. |
| `slice` | When slicing finishes | Result (`ok`, `error` or `cancelled`); how long it took; the number of plates, objects and copies; the printer, process and filament presets, named as in `preset_selected`; the printer's vendor; the material types (for example `PLA`); the **names** of up to 60 settings that differ from the built-in presets (for example `wall_loops`, never their values); which of a fixed list of features are on (non-planar printing, virtual 5-axis, Z anti-aliasing, wave overhangs, adaptive layer height, supports, spiral vase, prime tower, arc fitting, make-overhang-printable, brim ears, independent support layer height, cut planes, variable layer height, height-range modifiers, modifier parts, painted supports, seams, colours and fuzzy skin) and on how many copies; the chosen value of a fixed list of options with built-in choices (support type, wall generator, sparse infill pattern, slicing mode, print sequence, ironing type, fuzzy skin, seam position, timelapse type). If slicing failed: the type of error, and its message cut to the first line and 160 characters, with object, part and project names, quoted text and file paths removed. For errors in custom G-code templates only the type is sent, because the message would quote your own G-code. |
| `export` | When G-code is exported or sent | What kind: `gcode` (and whether to removable media), `gcode_3mf` (and whether all plates), or `send` (and the kind of printer connection, for example `octoprint`; never its address). |
| `crash` | At the next launch after the app closed unexpectedly | The version that crashed and roughly how long it had been running. |
| `session_end` | When the app closes | How long it was open, and counts of what was used during the session: tabs opened (`tab:<name>`), tools opened (`gizmo:<name>`, for example `gizmo:cut_planes`) and files opened by type (`import:stl`, `import:3mf`, ... or `import:other`). |

## On your computer

Diagnostics keep a few small files in the `usage` folder of the app's data folder:

- `session-<id>.json` while the app is running: its process id, version and start time, and the
  time it last sent diagnostics. It is deleted when the app closes normally. One left behind means
  that session crashed, which is reported as a `crash` event.
- `pending.json`: batches that could not be sent (offline, or the site was down), at most 20. They
  are retried at the next launch, and deleted when diagnostics are switched off.

## Sending and storage

Batches of at most 100 events go to `https://unlayered3d.com/api/slicer/telemetry` over HTTPS, every
few minutes and when the app closes. No sign-in is used, and nothing in a request identifies you.

On the site, the request's IP address is used only to limit how often one address can send and to
look up an approximate country. Neither the IP address nor any request header is stored. The
country is kept only as a daily count.

The site keeps daily aggregate counts (installs, sessions, events, versions, operating systems,
printers, features, countries) for 400 days. It also keeps the most recent batches as received, at
most 2,000 a day, for 30 days, to check that the data is sound. Only Unlayered 3D site
administrators can see either.

## Where it is in the code

- [`src/slic3r/Utils/UsageTelemetry.cpp`](src/slic3r/Utils/UsageTelemetry.cpp): batching, sending,
  crash detection, preset naming and error-message redaction
- [`src/slic3r/GUI/GUI_App.cpp`](src/slic3r/GUI/GUI_App.cpp): `app_start`, the first-run notice
- [`src/slic3r/GUI/Plater.cpp`](src/slic3r/GUI/Plater.cpp): `slice`, `export`, file types opened
- [`src/slic3r/GUI/Tab.cpp`](src/slic3r/GUI/Tab.cpp): `preset_selected`
- [`src/slic3r/GUI/MainFrame.cpp`](src/slic3r/GUI/MainFrame.cpp): tabs opened
- [`src/slic3r/GUI/Gizmos/GLGizmosManager.cpp`](src/slic3r/GUI/Gizmos/GLGizmosManager.cpp): tools opened
- [`src/slic3r/GUI/Preferences.cpp`](src/slic3r/GUI/Preferences.cpp): the on/off setting
