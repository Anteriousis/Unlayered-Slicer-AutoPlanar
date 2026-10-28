#!/usr/bin/env python3
"""Build a printer-configuration package (sys.zip) for the slicer's Config updater.

The slicer's Config button writes files into 0:/sys, 0:/macros and 0:/filaments on a Duet's SD
card. It reads them either from a folder or zip picked by hand, or from a zip attached to a
GitHub release. This script builds that zip, with a manifest.json inside it carrying the version,
the notes and a SHA-256 per file so a truncated download cannot be written to a printer.

Usage:
  python tools/make_sys_bundle.py \
      --sys      path/to/sys \
      --version  1.0.0 \
      --machine  "My printer" \
      --out      dist/sys.zip

  # optional extras
      --macros    path/to/macros
      --filaments path/to/filaments
      --notes-file CHANGES.md
      --include-machine-specific     # heightmap.csv and friends, normally left out
      --min-firmware 3.7.0-beta.1+unlayered.15

minFirmware, the oldest board firmware the files work on, is ENFORCED by the slicer: it refuses
to install onto an older board, because a config.g using a command that firmware does not know
stops at that line and the printer comes up with no heaters, fans or tool. Without
--min-firmware it is worked out from the files: sys/config.g's own `var fwNeed = N` (the same
number its first-boot firmware install tests), or +unlayered.12 for any M558 P12.

Then attach the zip to a release in the configuration repository. Without a manifest the updater
still works - it falls back to the release tag for the version and installs everything in the zip
- so this script is a convenience, not a requirement.
"""

import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import re
import sys
import zipfile

ALLOWED_ROOTS = ("sys", "macros", "filaments")

# Editor and backup droppings. The slicer ignores these too, but leaving them out of the package
# keeps the file list the user is shown honest.
IGNORE_PATTERNS = (
    "*.bak", "*.part", "*.tmp", "*.old", "*.orig", "*.swp",
    "~*", ".*", "Thumbs.db", "desktop.ini",
    # Written by the slicer to record what it installed; a copy pulled off a card would make the
    # next printer claim to be running whatever that card last had.
    "unlayered-sys.json",
)

# Files that describe one physical machine rather than the machine model: calibration that was
# measured on a specific printer, and the settings of whoever last used its web interface.
# Shipping these in a package would push one printer's calibration onto every other one.
MACHINE_SPECIFIC = (
    "heightmap.csv", "probePoints.csv", "config-override.g",
    "dwc-settings.json", "dwc-cache.json", "resurrect.g", "resurrect-prologue.g",
    # State a printer accumulates about itself, not configuration. Copying these from one card to
    # another does not just add clutter - printerstats.json is the odometer, so installing another
    # machine's copy silently rewrites this machine's lifetime totals, and the logs would read as
    # though events happened here that never did.
    "printerstats.json", "eventlog.txt", "piezolog.csv",
    # Written by the Duet configurator and by DWC to describe one installation.
    "configtool.json", "dwc-plugins.json",
    # The printer's own OVERLAYS: files config.g loads over its own defaults, written on the machine
    # by the panels, the setup wizard and calibration. Installing a copy would hand this machine
    # another one's plate offsets, name, network, probe polarity or measured tilt - or mark it as
    # set up when it never was. The printer's sys/factoryreset.g carries the full list.
    "plates.g", "machinename.g", "panelnet.g", "display.g", "setupdone.g", "piezopol.g",
    "callast.g", "cdefl.g", "filaments.csv", "panelfw.g", "fwflash.g",
    # Describes the 4.3in panel image in THAT card's 0:/firmware (build.py generates it from the
    # .bin). Installed onto another card it names an image that card may not hold, and daemon.g
    # then pushes whatever panel binary is actually there - possibly an older one.
    "panelimage.g",
    # Setup progress (Do not ask again, resume point) and the Z stop's measured tip height: state
    # sys/factoryreset.g erases. The slicer refuses to install any name on this list from any
    # source (SysFilesBundle.cpp sys_machine_specific_names; verify_sys_updater.py checks they agree).
    "setupskip.g", "setupprog.g", "zstall.g",
)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ignored(name, include_machine_specific):
    for pattern in IGNORE_PATTERNS:
        if fnmatch.fnmatch(name, pattern):
            return "matches %s" % pattern
    if not include_machine_specific and name.lower() in [m.lower() for m in MACHINE_SPECIFIC]:
        return "specific to one machine"
    return None


def collect(root_dir, card_root, include_machine_specific, skipped):
    """Walk a folder and yield (card-relative path, absolute path) pairs."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in sorted(filenames):
            abs_path = os.path.join(dirpath, name)
            rel = os.path.relpath(abs_path, root_dir).replace(os.sep, "/")
            reason = ignored(name, include_machine_specific)
            if reason:
                skipped.append(("%s/%s" % (card_root, rel), reason))
                continue
            if os.path.getsize(abs_path) == 0:
                # rr_upload has no way to create a zero length file.
                skipped.append(("%s/%s" % (card_root, rel), "empty"))
                continue
            out.append(("%s/%s" % (card_root, rel), abs_path))
    return out


def fork_number(version):
    """N of '...+unlayered.N', or -1."""
    m = re.search(r"\+unlayered\.(\d+)", version or "")
    return int(m.group(1)) if m else -1


def infer_min_firmware(files):
    """The same rule as the slicer's sys_required_firmware(), plus config.g's declared fwNeed.
    Returns (version, reason) or ("", "")."""
    best, why = -1, ""
    for card_path, abs_path in files:
        with open(abs_path, "rb") as f:
            data = f.read()
        if b"\0" in data:
            continue
        for line in data.decode("utf-8", "replace").splitlines():
            code = line.split(";", 1)[0].strip()
            words = code.split()
            if card_path == "sys/config.g":
                m = re.match(r"var\s+fwNeed\s*=\s*(\d+)\s*$", code)
                if m and int(m.group(1)) > best:
                    best, why = int(m.group(1)), "sys/config.g declares fwNeed"
            if words and words[0].upper() == "M558" and any(w.upper() == "P12" for w in words[1:]) and best < 12:
                best, why = 12, "%s uses M558 P12, the differential Z probe" % card_path
    return ("+unlayered.%d" % best, why) if best >= 0 else ("", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sys", help="Folder to install as 0:/sys")
    ap.add_argument("--macros", help="Folder to install as 0:/macros")
    ap.add_argument("--filaments", help="Folder to install as 0:/filaments")
    ap.add_argument("--version", required=True, help="Package version, e.g. 1.0.0")
    ap.add_argument("--date", help="Package date; defaults to today")
    ap.add_argument("--machine", default="", help="Which machine this is for, e.g. \"My printer\"")
    ap.add_argument("--notes", default="", help="Notes shown in the update dialog")
    ap.add_argument("--notes-file", help="Read the notes from a file")
    ap.add_argument("--include-machine-specific", action="store_true",
                    help="Include heightmap.csv, config-override.g and the like")
    ap.add_argument("--min-firmware", help="Oldest board firmware these files work on; inferred from the files when omitted")
    ap.add_argument("--out", default=os.path.join("dist", "sys.zip"), help="Output zip")
    args = ap.parse_args()

    if not (args.sys or args.macros or args.filaments):
        sys.exit("error: pass at least one of --sys, --macros, --filaments")

    skipped = []
    files = []
    for card_root, folder in (("sys", args.sys), ("macros", args.macros), ("filaments", args.filaments)):
        if not folder:
            continue
        if not os.path.isdir(folder):
            sys.exit("error: %s is not a folder" % folder)
        files.extend(collect(folder, card_root, args.include_machine_specific, skipped))

    if not files:
        sys.exit("error: nothing to package")

    notes = args.notes
    if args.notes_file:
        with open(args.notes_file, "r", encoding="utf-8", errors="replace") as f:
            notes = f.read()

    inferred, why = infer_min_firmware(files)
    if args.min_firmware:
        min_firmware = args.min_firmware
        if fork_number(inferred) > fork_number(min_firmware):
            sys.exit("error: --min-firmware %s is older than the files need (%s: %s)" % (min_firmware, inferred, why))
    else:
        min_firmware = inferred

    manifest = {
        "version": args.version,
        "date": args.date or datetime.date.today().strftime("%Y-%m-%d"),
        "machine": args.machine,
        "notes": notes,
        "files": [{"path": card_path, "sha256": sha256_of(abs_path)} for card_path, abs_path in files],
    }
    if min_firmware:
        manifest["minFirmware"] = min_firmware

    out_path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        for card_path, abs_path in files:
            z.write(abs_path, card_path)

    for (card_path, abs_path), entry in zip(files, manifest["files"]):
        print("  %-44s %8d bytes  %s" % (card_path, os.path.getsize(abs_path), entry["sha256"][:16] + "..."))
    for path, reason in skipped:
        print("  skipped %-36s (%s)" % (path, reason))

    print("\nwrote %s" % out_path)
    print("version %s (%s), %d file(s)" % (manifest["version"], manifest["date"], len(files)))
    if min_firmware:
        print("minFirmware %s (%s)" % (min_firmware, why if not args.min_firmware else "--min-firmware"))
    else:
        print("minFirmware: none - nothing in these files needs a particular firmware")


if __name__ == "__main__":
    main()
