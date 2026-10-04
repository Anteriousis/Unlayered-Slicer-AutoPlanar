# Bundled printer firmware

This directory is the **offline fallback** for the printer firmware updater
(`src/slic3r/Utils/FirmwareBundle.cpp`). The updater prefers a GitHub release and only reads this
directory when that is unreachable.

## The binaries are not in git

Firmware images are several megabytes each and would accumulate in the repository with every
release. Only this README and the generator script are tracked; `manifest.json` and the image
files are ignored (see `.gitignore`).

Populate the directory as part of cutting an installer:

```
python tools/make_firmware_bundle.py \
    --build-dir path/to/RepRapFirmware/Duet3Mini5plus \
    --iap       path/to/RepRapFirmware/firmware/Duet3_SDiap32_Mini5plus.bin \
    --wifi      path/to/RepRapFirmware/firmware/DuetWiFiServer.bin \
    --paneldue  path/to/PanelDueFirmware.bin \
    --panel-ota path/to/unlayered-panel.bin \
    --board     Mini5plus
```

Both display images belong in every release. Only one of them is ever flashed to a given machine
- the updater asks the printer which display it has (`global.displayId`, set by
`0:/sys/displayid.g`) and sends only the matching image - but which machine is on the other end
is not known at the time a release is cut.

The version is read out of `src/Version.h` in the firmware repository so the manifest cannot
drift from the image it describes.

## Testing this directory

The updater prefers a GitHub release, so a populated bundle is normally never read. To force it,
set the undocumented `github_api_base` key in the `app` section of
`%APPDATA%\Unlayered Slicer\Unlayered Slicer.conf` **while the application is closed** (it
rewrites the file on exit):

```json
"github_api_base": "http://127.0.0.1:9",
```

Port 9 refuses connections immediately, so the GitHub source fails fast rather than burning its
ten-second connect timeout, and the chain falls through to this directory. Point the key at a
real static server serving GitHub-shaped release JSON to exercise that path instead. Remove the
key to go back to the live releases.

## Manifest format

The same format is read from a `manifest.json` asset attached to a GitHub release, so the output
of the generator can be uploaded to a release unchanged.

```json
{
  "version": "3.7.0-beta.1+unlayered.1",
  "buildDate": "2026-07-28",
  "board": "Mini5plus",
  "notes": "Release notes shown in the update dialog.",
  "images": [
    { "role": "iap",       "file": "Duet3_SDiap32_Mini5plus.bin", "sha256": "..." },
    { "role": "mainboard", "file": "Duet3Firmware_Mini5plus.uf2", "sha256": "..." },
    { "role": "wifi",      "file": "DuetWiFiServer.bin",          "sha256": "..." },
    { "role": "paneldue",  "file": "PanelDueFirmware.bin",        "sha256": "..." },
    { "role": "panelota",  "file": "unlayered-panel.bin",         "sha256": "..." }
  ]
}
```

`role` is one of `mainboard`, `iap`, `wifi`, `paneldue`, `panelota`. It maps to the M997 `S`
parameter (`mainboard` = 0, `wifi` = 1, `paneldue` = 4, `panelota` = 5); `iap` is the
in-application programmer that performs the mainboard flash and is never an M997 target itself.
`panelota` is the Unlayered 4.3in ESP32 panel, which writes its inactive OTA slot and rolls back
on its own; `paneldue` goes through SAM-BA, which erases before it receives.

**Display images must carry the names RRF looks for** - `PanelDueFirmware.bin` and
`unlayered-panel.bin`, both compile-time constants in the firmware's `Configuration.h`. The
generator renames them for you. This is not cosmetic: `M997` accepts a `P"filename"` only when
exactly one module is being updated ("Filename can only be provided when updating exactly one
module"), so a combined flash such as `M997 S0:5` can only find the image if the file already
carries RRF's own name.

**Only a mainboard image reboots the board.** RRF's `flashing1` state runs every auxiliary module
to completion and only then enters `flashing2`, which is the state that restarts. So one `M997`
carrying several modules is already correctly ordered, and a display-only flash never drops the
connection.

**The IAP must always be included alongside a mainboard image.** `M997 S0` fails outright if
`Duet3_SDiap32_<board>.bin` is not present in `0:/firmware/`, and there is no way to tell whether
the copy already on the SD card matches the firmware being installed.

A release without a `manifest.json` still works: the updater falls back to matching asset names
against the filenames the board reports for itself (`boards[0].firmwareFileName`,
`iapFileNameSD`, `wifiFirmwareFileName`), so attaching the raw `.uf2` and IAP to a release is
enough. The manifest is what adds checksums and an authoritative version string.
