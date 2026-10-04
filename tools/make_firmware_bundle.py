#!/usr/bin/env python3
"""Populate resources/firmware/ with a firmware bundle for the slicer to ship.

The bundled firmware is the offline fallback: the updater prefers a GitHub release and only
falls back to what shipped with the installer. Because of that the binaries are deliberately
NOT committed to the slicer repository - they would add megabytes per release forever. Run this
script as part of cutting an installer, or locally when you want the fallback populated.

The manifest format produced here is the same one the updater reads from a GitHub release
asset, so the output of this script can also be uploaded to a release verbatim.

Usage:
  python tools/make_firmware_bundle.py \
      --build-dir C:/rrf/RepRapFirmware/Duet3Mini5plus \
      --iap       C:/rrf/RepRapFirmware/firmware/Duet3_SDiap32_Mini5plus.bin \
      --version   3.7.0-beta.1+unlayered.1 \
      --board     Mini5plus

  # optional extras
      --wifi      C:/rrf/RepRapFirmware/firmware/DuetWiFiServer.bin
      --notes-file CHANGELOG.md
      --out       resources/firmware
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys

UF2_MAGIC_0 = 0x0A324655
UF2_MAGIC_1 = 0x9E5D5157


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_uf2(path):
    """The board rejects a bad image only after it has been written to the SD card, so verify the
    UF2 header here rather than discovering the problem on the printer."""
    with open(path, "rb") as f:
        head = f.read(8)
    if len(head) < 8:
        return False
    m0 = int.from_bytes(head[0:4], "little")
    m1 = int.from_bytes(head[4:8], "little")
    return m0 == UF2_MAGIC_0 and m1 == UF2_MAGIC_1


def version_from_source(repo_root):
    """Read MAIN_VERSION/FORK_VERSION out of src/Version.h so the manifest cannot drift from the
    firmware it describes."""
    version_h = os.path.join(repo_root, "src", "Version.h")
    try:
        with open(version_h, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    main = re.search(r'#\s*define\s+MAIN_VERSION\s+"([^"]+)"', text)
    fork = re.search(r'#\s*define\s+FORK_VERSION\s+"([^"]+)"', text)
    if not main:
        return None
    return main.group(1) + (fork.group(1) if fork else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, help="RRF per-board build directory containing the .uf2")
    ap.add_argument("--iap", required=True, help="Path to the Duet3_SDiap32_*.bin in-application programmer")
    ap.add_argument("--wifi", help="Optional WiFi module firmware (DuetWiFiServer.bin / DuetWiFiModule_32S3.bin)")
    ap.add_argument("--paneldue", help="Optional PanelDue firmware (M997 S4); published as PanelDueFirmware.bin")
    ap.add_argument("--panel-ota", help="Optional Unlayered 4.3in panel image (M997 S5); published as unlayered-panel.bin")
    ap.add_argument("--board", default="Mini5plus", help="Board short name as reported in boards[0].shortName")
    ap.add_argument("--version", help="Firmware version string; read from src/Version.h when omitted")
    ap.add_argument("--build-date", help="Build date; defaults to the .uf2 file's mtime")
    ap.add_argument("--notes", default="", help="Release notes text")
    ap.add_argument("--notes-file", help="Read release notes from a file")
    ap.add_argument("--out", default=os.path.join("resources", "firmware"), help="Output directory")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    main_name = "Duet3Firmware_%s.uf2" % args.board
    main_path = os.path.join(build_dir, main_name)

    if not os.path.isfile(main_path):
        sys.exit("error: mainboard firmware not found: %s" % main_path)
    if not os.path.isfile(args.iap):
        sys.exit("error: IAP binary not found: %s" % args.iap)
    if not check_uf2(main_path):
        sys.exit("error: %s does not have a UF2 header - is it really a Mini 5+ image?" % main_path)

    version = args.version
    if not version:
        # build_dir is <repo>/<BoardBuildDir>, so the repo root is one level up.
        version = version_from_source(os.path.dirname(build_dir))
        if not version:
            sys.exit("error: could not determine the version; pass --version")

    build_date = args.build_date
    if not build_date:
        build_date = datetime.datetime.fromtimestamp(
            os.path.getmtime(main_path), datetime.timezone.utc).strftime("%Y-%m-%d")

    notes = args.notes
    if args.notes_file:
        with open(args.notes_file, "r", encoding="utf-8", errors="replace") as f:
            notes = f.read()

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    # The IAP is listed first: it is uploaded first so a partial failure never leaves new
    # firmware paired with a stale programmer.
    entries = [("iap", args.iap, os.path.basename(args.iap)),
               ("mainboard", main_path, main_name)]
    if args.wifi:
        if not os.path.isfile(args.wifi):
            sys.exit("error: WiFi firmware not found: %s" % args.wifi)
        entries.append(("wifi", args.wifi, os.path.basename(args.wifi)))

    # Display images are published under the names RRF looks for, whatever they were called on
    # the way in. This is not cosmetic: M997 accepts a P"filename" only when exactly one module is
    # being updated, so a combined flash (S0:4 or S0:5) can only find the image if the file on the
    # card already carries RRF's own name. Both constants are compile-time in Configuration.h.
    for flag, role, target in (("paneldue", "paneldue", "PanelDueFirmware.bin"),
                               ("panel_ota", "panelota", "unlayered-panel.bin")):
        src = getattr(args, flag)
        if not src:
            continue
        if not os.path.isfile(src):
            sys.exit("error: %s firmware not found: %s" % (role, src))
        if os.path.basename(src) != target:
            print("  note: publishing %s as %s, the name RRF looks for"
                  % (os.path.basename(src), target))
        entries.append((role, src, target))

    images = []
    for role, src, name in entries:
        dst = os.path.join(out_dir, name)
        if os.path.abspath(src) != dst:
            shutil.copy2(src, dst)
        digest = sha256_of(dst)
        images.append({"role": role, "file": name, "sha256": digest})
        print("  %-10s %-40s %9d bytes  %s" % (role, name, os.path.getsize(dst), digest[:16] + "..."))

    manifest = {
        "version": version,
        "buildDate": build_date,
        "board": args.board,
        "notes": notes,
        "images": images,
    }

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print("\nwrote %s" % manifest_path)
    print("version %s (built %s) for %s" % (version, build_date, args.board))


if __name__ == "__main__":
    main()
