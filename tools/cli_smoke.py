#!/usr/bin/env python3
"""Headless CLI regression tests — the crashes that no unit test can reach.

`PartPlate` and the plate-rebuild path live in `libslic3r_gui`, but `libslic3r_tests`
links only `libslic3r`, so the code that runs between "model loaded" and "action
performed" in CLI mode has no unit-test coverage at all. It is also exactly where
GUI-only assumptions hide: `PartPlate::m_plater` is null headlessly, and roughly twenty
sites in PartPlate.cpp guard for that while others historically did not.

This harness drives the real binary and asserts it does not die. Each case builds its
mesh parametrically, so there are no binary fixtures to keep in sync.

    python tools/cli_smoke.py --exe build/src/Release/unlayered-slicer.exe

Add a case whenever a headless crash is fixed, so it cannot come back silently.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


def write_cube_stl(path, z_min, z_max, size=10.0):
    """A closed, manifold, correctly wound box. 12 facets."""
    h = size / 2.0
    v = [(-h, -h, z_min), (h, -h, z_min), (h, h, z_min), (-h, h, z_min),
         (-h, -h, z_max), (h, -h, z_max), (h, h, z_max), (-h, h, z_max)]
    faces = [(0, 3, 2), (0, 2, 1), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    d = bytearray(b"\x00" * 80 + struct.pack("<I", len(faces)))
    for a, b, c in faces:
        d += struct.pack("<3f", 0.0, 0.0, 0.0)  # normal recomputed on load
        for i in (a, b, c):
            d += struct.pack("<3f", *v[i])
        d += b"\x00\x00"
    with open(path, "wb") as f:
        f.write(bytes(d))


# Each case: (name, mesh builder, CLI args, why it exists)
CASES = [
    (
        "sinking object",
        lambda p: write_cube_stl(p, -5.0, 5.0),
        ["--info"],
        # PartPlate::check_outside() takes a separate branch for a SINKING instance
        # (bbox min z < SINKING_Z_THRESHOLD = -0.001), and that branch alone read
        # m_plater->build_volume().printable_height(). m_plater is null in CLI mode, so
        # ANY model extending below the bed took the whole slicer down with an
        # ACCESS_VIOLATION on a null read — reached via
        # rebuild_plates_after_arrangement -> reload_all_objects -> add_instance ->
        # check_outside. Found on a 137k-face manifold, but a 12-facet cube straddling
        # z=0 reproduces it exactly; the mesh was never the point.
        "a model below the bed must not crash the CLI",
    ),
    (
        "object resting on the bed",
        lambda p: write_cube_stl(p, 0.0, 10.0),
        ["--info"],
        # Also pins PartPlate::belt_open_y(): check_outside() asks it for every instance, and it
        # reads the printer preset through wxGetApp(), which has no wxApp behind it in CLI mode.
        # The guard on m_plater (null headlessly) is what keeps this from segfaulting.
        "control for the case above: this one never took the sinking branch; also the belt guard",
    ),
    (
        "sinking object, arrange forced on",
        lambda p: write_cube_stl(p, -5.0, 5.0),
        ["--arrange", "1", "--info"],
        "--arrange 0 was the workaround, so pin the path that actually crashed",
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="path to unlayered-slicer.exe")
    ap.add_argument("--datadir", default=None, help="slicer datadir (default: %APPDATA%/Unlayered Slicer)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    datadir = args.datadir
    if datadir is None:
        appdata = os.environ.get("APPDATA", "")
        datadir = os.path.join(appdata, "Unlayered Slicer")

    # Absolute, or CreateProcess fails on Windows even when the relative path resolves for us.
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        sys.exit("no such exe: %s" % exe)

    failures = 0
    with tempfile.TemporaryDirectory(prefix="cli_smoke_") as tmp:
        for name, build, cli_args, why in CASES:
            mesh = os.path.join(tmp, name.replace(" ", "_").replace(",", "") + ".stl")
            build(mesh)
            cmd = [exe, "--datadir", datadir] + cli_args + [mesh]
            proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
            rc = proc.returncode
            # On Windows a crash surfaces as the negative NTSTATUS; 0xC0000005 is -1073741819.
            crashed = rc != 0
            if crashed:
                failures += 1
                print("FAIL  %-34s exit=%d (0x%08X)" % (name, rc, rc & 0xFFFFFFFF))
                print("        %s" % why)
                if args.verbose:
                    tail = (proc.stdout or "").strip().splitlines()[-6:]
                    for line in tail:
                        print("        | %s" % line)
            else:
                print("PASS  %-34s exit=0" % name)
                if args.verbose:
                    print("        %s" % why)

    print()
    print("%d passed, %d failed" % (len(CASES) - failures, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
