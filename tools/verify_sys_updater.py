#!/usr/bin/env python3
"""Verify the assumptions the printer-configuration updater is built on.

Companion to verify_firmware_updater.py, and the same idea: these are checks of protocol and data
semantics, not of the C++ itself. Each corresponds to a decision in
src/slic3r/Utils/SysFilesBundle.cpp, src/slic3r/Utils/DuetRrfClient.cpp or
src/slic3r/GUI/Jobs/SysFilesUpdateJob.cpp that would quietly do the wrong thing to a printer's
configuration if the assumption were wrong.

  python tools/verify_sys_updater.py
  python tools/verify_sys_updater.py --probe 192.168.1.80   # read-only, talks to a printer

The probe never writes a file. It calls rr_mkdir on a directory that already exists, which RRF
answers with err=1 and no change on the card - that answer is itself one of the assumptions.
"""

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUNDLE_CPP = os.path.join(REPO, "src", "slic3r", "Utils", "SysFilesBundle.cpp")
GENERATOR = os.path.join(REPO, "tools", "make_sys_bundle.py")

_passed = 0
_failed = 0


def check(name, ok, detail=""):
    global _passed, _failed
    if ok:
        _passed += 1
        print("  PASS  %s" % name)
    else:
        _failed += 1
        print("  FAIL  %s%s" % (name, ("\n          " + detail) if detail else ""))
    return ok


# ---------------------------------------------------------------------------------------------
# 1. The generator and the reader have to agree
# ---------------------------------------------------------------------------------------------
def test_generator_agreement():
    """make_sys_bundle.py writes the zip; SysFilesBundle.cpp reads it. The layout, the manifest
    key names and the ignore rules live in both, so they can drift apart silently."""
    print("\n[Generator and reader agree]")

    if not check("SysFilesBundle.cpp is present", os.path.isfile(BUNDLE_CPP)):
        return
    with open(BUNDLE_CPP, "r", encoding="utf-8", errors="replace") as f:
        cpp = f.read()

    for root in ("sys", "macros", "filaments"):
        check("C++ allows the %s root" % root, ('"%s"' % root) in cpp)

    # The reader looks for these exact keys. A rename on either side loses the version silently,
    # which then reads as "no version declared" rather than as an error.
    for key in ("version", "date", "machine", "notes", "files", "sha256"):
        check("C++ reads the manifest key %r" % key, ('"%s"' % key) in cpp)

    check("C++ treats manifest.json at the package root as the manifest",
          '"manifest.json"' in cpp)

    # Both sides skip these, so a card image dropped into a package does not carry junk onto a
    # printer. Only the extension list is compared; that is where drift actually happens.
    with open(GENERATOR, "r", encoding="utf-8", errors="replace") as f:
        gen = f.read()
    for ext in (".bak", ".part", ".tmp", ".old", ".orig", ".swp"):
        check("both sides ignore %s" % ext, ('"%s"' % ext) in cpp and ('"*%s"' % ext) in gen)

    check("both sides refuse to carry the installed-version marker",
          "unlayered-sys.json" in cpp and "unlayered-sys.json" in gen)


def test_package_roundtrip():
    """Build a package the way a release would, then read it back the way the slicer does."""
    print("\n[Package round trip]")

    with tempfile.TemporaryDirectory() as tmp:
        sys_dir = os.path.join(tmp, "sys")
        os.makedirs(os.path.join(sys_dir, "accelerometer"))
        with open(os.path.join(sys_dir, "config.g"), "w", newline="\r\n") as f:
            f.write("; config\r\nM550 P\"Test\"\r\n")
        with open(os.path.join(sys_dir, "homeall.g"), "w", newline="\n") as f:
            f.write("G91\nG1 H2 Z5\n")
        # Left out on both sides: junk, a machine specific calibration file, an empty file.
        with open(os.path.join(sys_dir, "config.g.bak"), "w") as f:
            f.write("old\n")
        with open(os.path.join(sys_dir, "heightmap.csv"), "w") as f:
            f.write("RepRapFirmware height map\n")
        with open(os.path.join(sys_dir, "empty.g"), "w"):
            pass

        out = os.path.join(tmp, "sys.zip")
        proc = subprocess.run([sys.executable, GENERATOR, "--sys", sys_dir,
                               "--version", "1.2.3", "--machine", "Test", "--out", out],
                              capture_output=True, text=True)
        if not check("make_sys_bundle.py runs", proc.returncode == 0, proc.stderr.strip()):
            return

        with zipfile.ZipFile(out) as z:
            names = z.namelist()
            manifest = json.loads(z.read("manifest.json").decode("utf-8"))
            config = z.read("sys/config.g")

        check("the manifest sits at the root of the zip", "manifest.json" in names)
        check("paths in the zip are card relative with forward slashes",
              "sys/config.g" in names and "sys/homeall.g" in names,
              str(names))
        check("no backslashes in any entry", not any("\\" in n for n in names), str(names))
        check("the manifest declares the version", manifest.get("version") == "1.2.3")
        check("every file in the zip is declared in the manifest",
              sorted(e["path"] for e in manifest["files"]) ==
              sorted(n for n in names if n != "manifest.json"))
        check("backup files are left out", not any(n.endswith(".bak") for n in names))
        check("machine specific calibration is left out by default",
              "sys/heightmap.csv" not in names)
        check("empty files are left out (rr_upload cannot create one)",
              "sys/empty.g" not in names)
        # RRF reads either line ending, and the uploader must not touch the bytes: a CRLF file has
        # to arrive as CRLF, which is exactly what the Windows text-mode trap used to break.
        check("CRLF survives packaging byte for byte", b"\r\n" in config)


# ---------------------------------------------------------------------------------------------
# 2. Protocol, against a real printer (read only)
# ---------------------------------------------------------------------------------------------
def probe(host):
    print("\n[Live printer %s - read only]" % host)
    try:
        import urllib.request
        import urllib.error
    except ImportError:
        check("urllib is available", False)
        return

    base = "http://%s/" % host

    def get(path, timeout=10):
        req = urllib.request.Request(base + path, headers={"User-Agent": "verify_sys_updater"})
        return urllib.request.urlopen(req, timeout=timeout)

    try:
        body = get("rr_connect?password=reprap&sessionKey=yes").read().decode("utf-8")
        conn = json.loads(body)
    except Exception as e:
        check("rr_connect succeeds", False, str(e))
        return
    if not check("rr_connect succeeds", conn.get("err") == 0, body):
        return

    key = conn.get("sessionKey")
    check("the board hands out a session key (RRF >= 3.5)", key is not None)

    # network.name is what the dialog labels the printer with; boards[0].shortName is the same on
    # every Unlayered machine, which is why the name matters.
    try:
        model = json.loads(get("rr_model?key=network&flags=v").read().decode("utf-8"))
        name = model.get("result", {}).get("name")
        check("network.name is the machine name from M550", bool(name), str(model.get("result"))[:200])
        print("          machine name: %r" % name)
    except Exception as e:
        check("rr_model(network) works", False, str(e))

    # rr_filelist is how the updater could enumerate what is on the card, and the shape of the
    # response is what DuetRrfClient::filelist parses.
    try:
        listing = json.loads(get("rr_filelist?dir=0:/sys&first=0").read().decode("utf-8"))
        check("rr_filelist(0:/sys) succeeds", listing.get("err", 0) == 0, str(listing)[:200])
        files = listing.get("files", [])
        check("the listing has entries", len(files) > 0)
        if files:
            first = files[0]
            check("entries carry type/name/size", all(k in first for k in ("type", "name", "size")), str(first))
            check("the cursor field is present", "next" in listing)
        # err=2 is "no such directory", which the client treats as an answer rather than a failure.
        missing = json.loads(get("rr_filelist?dir=0:/definitely-not-here").read().decode("utf-8"))
        check("a missing directory answers err=2", missing.get("err") == 2, str(missing))
    except Exception as e:
        check("rr_filelist works", False, str(e))

    # rr_download is used to back up each file before it is overwritten, and to read the marker.
    try:
        data = get("rr_download?name=0:/sys/config.g").read()
        check("rr_download returns config.g", len(data) > 0, "%d bytes" % len(data))
    except Exception as e:
        check("rr_download returns config.g", False, str(e))

    try:
        get("rr_download?name=0:/sys/definitely-not-here.json").read()
        check("a missing file answers 404", False, "the request succeeded")
    except Exception as e:
        code = getattr(e, "code", None)
        check("a missing file answers 404", code == 404, "HTTP %s" % code)

    # The updater calls rr_mkdir before uploading and ignores the result, because RRF cannot tell
    # "already there" apart from a failure. Confirm that is what it does. Nothing is created: the
    # directory already exists.
    try:
        url = "rr_mkdir?dir=0:/sys"
        if key is not None:
            url += "&sessionKey=%s" % key
        req = urllib.request.Request(base + url, headers={"User-Agent": "verify_sys_updater",
                                                          "X-Session-Key": str(key)})
        res = json.loads(urllib.request.urlopen(req, timeout=10).read().decode("utf-8"))
        check("rr_mkdir on an existing directory answers err=1 (so its result is advisory)",
              res.get("err") == 1, str(res))
    except Exception as e:
        check("rr_mkdir answers", False, str(e))

    try:
        get("rr_disconnect").read()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", metavar="HOST", help="Also run read-only checks against a printer")
    args = ap.parse_args()

    test_generator_agreement()
    test_package_roundtrip()
    if args.probe:
        probe(args.probe)

    print("\n%d passed, %d failed" % (_passed, _failed))
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
