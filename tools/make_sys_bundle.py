#!/usr/bin/env python3
"""Build a printer-configuration package (sys.zip) for the slicer's Config updater.

The slicer's Config button writes files into 0:/sys, 0:/macros and 0:/filaments on a Duet's SD
card. It reads them either from a folder or zip picked by hand, or from a zip attached to a
GitHub release. This script builds that zip, with a manifest.json inside it carrying the version,
the notes and a SHA-256 per file so a truncated download cannot be written to a printer.

Usage:
  python tools/make_sys_bundle.py \
      --sys      C:/rrf/RepRapFirmware/sys \
      --version  1.0.0 \
      --machine  "Unlayered .80" \
      --out      dist/sys.zip

  # optional extras
      --macros    C:/path/to/macros
      --filaments C:/path/to/filaments
      --notes-file CHANGES.md
      --include-machine-specific     # heightmap.csv and friends, normally left out

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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sys", help="Folder to install as 0:/sys")
    ap.add_argument("--macros", help="Folder to install as 0:/macros")
    ap.add_argument("--filaments", help="Folder to install as 0:/filaments")
    ap.add_argument("--version", required=True, help="Package version, e.g. 1.0.0")
    ap.add_argument("--date", help="Package date; defaults to today")
    ap.add_argument("--machine", default="", help="Which machine this is for, e.g. \"Unlayered .80\"")
    ap.add_argument("--notes", default="", help="Notes shown in the update dialog")
    ap.add_argument("--notes-file", help="Read the notes from a file")
    ap.add_argument("--include-machine-specific", action="store_true",
                    help="Include heightmap.csv, config-override.g and the like")
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

    manifest = {
        "version": args.version,
        "date": args.date or datetime.date.today().strftime("%Y-%m-%d"),
        "machine": args.machine,
        "notes": notes,
        "files": [{"path": card_path, "sha256": sha256_of(abs_path)} for card_path, abs_path in files],
    }

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


if __name__ == "__main__":
    main()
