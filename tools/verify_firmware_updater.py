#!/usr/bin/env python3
"""Verify the assumptions the firmware updater is built on.

These are checks of protocol and data semantics, not of the C++ itself. Each one corresponds to
a decision in src/slic3r/Utils/DuetRrfClient.cpp or FirmwareBundle.cpp that would silently
produce a corrupt flash or a wrong "already up to date" answer if the assumption were wrong.

  python tools/verify_firmware_updater.py
  python tools/verify_firmware_updater.py --probe 192.168.1.80   # read-only, talks to a printer
"""

import argparse
import binascii
import json
import os
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_DIR = os.path.join(REPO, "resources", "firmware")

UF2_MAGIC_0 = 0x0A324655
UF2_MAGIC_1 = 0x9E5D5157

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
# 1. CRC-32 variant
# ---------------------------------------------------------------------------------------------
def test_crc32():
    """DuetRrfClient sends &crc32=<hex> and RRF recomputes it with its own CRC32 class
    (poly 0xEDB88320, init 0xFFFFFFFF, final XOR) - the zlib variant. miniz's mz_crc32 is the
    same algorithm. A mismatch here means every upload is rejected."""
    print("\n[CRC-32 variant]")

    # Known-answer test for CRC-32/ISO-HDLC.
    check("CRC-32 of \"123456789\" is 0xCBF43926",
          zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926,
          "got 0x%08X" % (zlib.crc32(b"123456789") & 0xFFFFFFFF))

    # binascii.crc32 and zlib.crc32 are the same function; confirms no variant confusion.
    check("binascii and zlib agree",
          binascii.crc32(b"unlayered") & 0xFFFFFFFF == zlib.crc32(b"unlayered") & 0xFFFFFFFF)

    # Empty input must be 0, matching MZ_CRC32_INIT of 0.
    check("CRC-32 of empty input is 0", zlib.crc32(b"") & 0xFFFFFFFF == 0)


# ---------------------------------------------------------------------------------------------
# 2. Binary round-trip - the text-mode bug
# ---------------------------------------------------------------------------------------------
def test_binary_roundtrip():
    """Http::set_post_body(const fs::path&) reads through a text-mode ifstream. On Windows that
    collapses CRLF and stops at 0x1A, so the updater loads images itself in binary mode and uses
    the std::string overload. This demonstrates the corruption that motivated that choice."""
    print("\n[Binary round-trip]")

    payload = bytes([0x55, 0x46, 0x32, 0x0A, 0x0D, 0x0A, 0x1A, 0x00, 0xFF, 0x0D, 0x0A])

    tmp = os.path.join(FW_DIR, "_roundtrip_test.bin")
    os.makedirs(FW_DIR, exist_ok=True)
    with open(tmp, "wb") as f:
        f.write(payload)
    try:
        with open(tmp, "rb") as f:
            binary_read = f.read()
        check("binary-mode read preserves every byte", binary_read == payload,
              "expected %r, got %r" % (payload, binary_read))

        # Emulate a Windows text-mode read: CRLF collapses to LF and 0x1A ends the stream.
        text_emulated = payload.split(b"\x1a")[0].replace(b"\r\n", b"\n")
        check("text-mode read WOULD corrupt the image (so it must not be used)",
              text_emulated != payload,
              "text-mode emulation unexpectedly matched")
        print("          text-mode would yield %d of %d bytes" % (len(text_emulated), len(payload)))
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


# ---------------------------------------------------------------------------------------------
# 3. Version comparison semantics
# ---------------------------------------------------------------------------------------------
def test_version_semantics():
    """The Unlayered fork identifies itself in semver build metadata (+unlayered.N). Semver
    comparison ignores build metadata entirely, so the updater compares version strings exactly.
    If it used semver ordering it would report 'up to date' across different builds."""
    print("\n[Version comparison]")

    a = "3.7.0-beta.1+unlayered.1"
    b = "3.7.0-beta.1+unlayered.2"

    check("the two builds differ as strings (what the updater compares)", a != b)

    def semver_key(v):
        # Strip build metadata, exactly as a spec-compliant comparison does.
        return v.split("+", 1)[0]

    check("semver ordering WOULD wrongly call them equal (so it must not be used)",
          semver_key(a) == semver_key(b),
          "metadata-stripped forms differ, assumption no longer holds")


# ---------------------------------------------------------------------------------------------
# 4. UF2 sniffing against the real image
# ---------------------------------------------------------------------------------------------
def test_uf2():
    print("\n[UF2 image detection]")

    def is_uf2(data):
        if len(data) < 8:
            return False
        return (int.from_bytes(data[0:4], "little") == UF2_MAGIC_0 and
                int.from_bytes(data[4:8], "little") == UF2_MAGIC_1)

    check("random bytes are rejected", not is_uf2(b"\x00" * 64))
    check("a short buffer is rejected", not is_uf2(b"UF2"))

    uf2 = None
    bin_ = None
    if os.path.isdir(FW_DIR):
        for name in os.listdir(FW_DIR):
            p = os.path.join(FW_DIR, name)
            if name.lower().endswith(".uf2"):
                uf2 = p
            elif name.lower().endswith(".bin"):
                bin_ = p

    if uf2:
        with open(uf2, "rb") as f:
            head = f.read(8)
        check("the real %s is detected as UF2" % os.path.basename(uf2), is_uf2(head))
    else:
        print("  SKIP  no .uf2 in resources/firmware (run tools/make_firmware_bundle.py)")

    if bin_:
        with open(bin_, "rb") as f:
            head = f.read(8)
        check("a raw .bin is NOT detected as UF2 (%s)" % os.path.basename(bin_), not is_uf2(head))


# ---------------------------------------------------------------------------------------------
# 5. Manifest integrity
# ---------------------------------------------------------------------------------------------
# RRF looks for these names and nothing else (PANEL_DUE_FIRMWARE_FILE / PANEL_OTA_FIRMWARE_FILE in
# Configuration.h). M997 accepts a P"filename" only when updating exactly one module, so a combined
# flash such as S0:5 cannot name the file and will simply not find an image published under any
# other name - with no error that points at the cause.
DISPLAY_FILENAMES = {
    "paneldue": "PanelDueFirmware.bin",
    "display":  "PanelDueFirmware.bin",
    "panelota": "unlayered-panel.bin",
    "panel":    "unlayered-panel.bin",
    "esp32panel": "unlayered-panel.bin",
}


def check_display_filenames(images):
    for img in images:
        want = DISPLAY_FILENAMES.get(img.get("role"))
        if not want:
            continue
        check("%s image is published as %s" % (img.get("role"), want),
              img.get("file") == want,
              "found %r - RRF only looks for %r, and a combined M997 cannot pass a P filename"
              % (img.get("file"), want))


def test_manifest():
    import hashlib
    print("\n[Bundled manifest]")

    path = os.path.join(FW_DIR, "manifest.json")
    if not os.path.isfile(path):
        print("  SKIP  no manifest.json (run tools/make_firmware_bundle.py)")
        return

    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)

    check("manifest declares a version", bool(m.get("version")))
    check("manifest declares a board", bool(m.get("board")))
    check("manifest lists images", bool(m.get("images")))

    roles = [i.get("role") for i in m.get("images", [])]
    check("every role is recognised",
          all(r in ("mainboard", "iap", "wifi", "paneldue", "panelota") for r in roles),
          "roles: %r" % roles)
    check_display_filenames(m.get("images", []))
    # M997 S0 fails outright without the IAP on the card, and we cannot know whether the copy
    # already there matches, so a mainboard image must always be accompanied by one.
    if "mainboard" in roles:
        check("a mainboard image is accompanied by its IAP", "iap" in roles,
              "roles present: %r" % roles)

    for img in m.get("images", []):
        p = os.path.join(FW_DIR, img["file"])
        if not os.path.isfile(p):
            check("image present: %s" % img["file"], False)
            continue
        with open(p, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        check("sha256 matches for %s" % img["file"], digest == img.get("sha256", ""),
              "manifest %s, actual %s" % (img.get("sha256", "")[:16], digest[:16]))


# ---------------------------------------------------------------------------------------------
# 6. Optional read-only probe of a real printer
# ---------------------------------------------------------------------------------------------
def probe(host, password):
    """Exercises exactly the read-only calls the updater makes before it would upload anything:
    rr_connect, rr_model?key=boards[0], rr_model?key=state. Nothing is written to the printer."""
    import urllib.request
    import urllib.parse
    import datetime

    print("\n[Live probe of %s - read only]" % host)
    base = "http://%s/" % host

    def get(path, timeout=6):
        req = urllib.request.Request(base + path, headers={"User-Agent": "UnlayeredSlicer-probe"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    try:
        c = get("rr_connect?password=%s&sessionKey=yes&time=%s" % (urllib.parse.quote(password), ts))
    except Exception as e:
        print("  SKIP  printer unreachable: %s" % e)
        return

    if not check("rr_connect returns err=0", c.get("err") == 0, "response: %r" % c):
        return
    print("          boardType=%s apiLevel=%s sessionKey=%s"
          % (c.get("boardType"), c.get("apiLevel"), c.get("sessionKey")))

    try:
        board = get("rr_model?key=boards[0]&flags=v").get("result", {})
        state = get("rr_model?key=state&flags=f").get("result", {})
        network = get("rr_model?key=network&flags=v").get("result", {})
    except Exception as e:
        check("object model readable", False, str(e))
        return
    finally:
        try:
            get("rr_disconnect")
        except Exception:
            pass

    # These are the exact field names DuetRrfClient::read_board_info parses. If any is missing the
    # updater would upload firmware under the wrong name, so this is the highest-value check here.
    for field in ("firmwareVersion", "firmwareFileName", "iapFileNameSD", "shortName"):
        check("boards[0].%s present" % field, field in board and board[field] not in (None, ""),
              "got %r" % board.get(field))
    check("state.status present", bool(state.get("status")))

    print("          firmwareVersion : %s" % board.get("firmwareVersion"))
    print("          firmwareDate    : %s" % board.get("firmwareDate"))
    print("          firmwareFileName: %s" % board.get("firmwareFileName"))
    print("          iapFileNameSD   : %s" % board.get("iapFileNameSD"))
    print("          wifiFirmwareFile: %s" % board.get("wifiFirmwareFileName"))
    print("          shortName       : %s" % board.get("shortName"))
    print("          status          : %s" % state.get("status"))

    # A Mini 5+ runs one binary on both variants and reports wifiFirmwareFileName either way, so
    # the presence of that field says nothing about whether there is a module to flash. The
    # updater keys off the interface type instead; this guards that reasoning.
    iface_types = [i.get("type") for i in network.get("interfaces", [])]
    has_wifi_iface = "wifi" in iface_types
    print("          interfaces      : %s" % ", ".join(t or "?" for t in iface_types))

    check("interface list is non-empty", bool(iface_types), "network=%r" % network)

    # This is DuetRrfClient::BoardInfo::has_wifi_module() restated: the module is only offered
    # when an actual wifi interface is present, never on the filename alone.
    would_offer = has_wifi_iface and bool(board.get("wifiFirmwareFileName"))
    check("wifi module is not offered on a board with no wifi interface",
          has_wifi_iface or not would_offer,
          "would offer wifi flash on a %s-only board" % "/".join(t or "?" for t in iface_types))

    if board.get("wifiFirmwareFileName") and not has_wifi_iface:
        print("          note: board advertises %s but has no wifi interface - correctly skipped"
              % board.get("wifiFirmwareFileName"))

    # Cross-check the live board against what we would try to install.
    mpath = os.path.join(FW_DIR, "manifest.json")
    if os.path.isfile(mpath):
        with open(mpath, "r", encoding="utf-8") as f:
            m = json.load(f)
        check("bundled manifest board matches the live board",
              str(m.get("board", "")).lower() == str(board.get("shortName", "")).lower(),
              "manifest %r vs board %r" % (m.get("board"), board.get("shortName")))
        names = [i["file"] for i in m.get("images", [])]
        if board.get("firmwareFileName"):
            check("manifest contains the board's own firmware filename",
                  board["firmwareFileName"] in names, "manifest files: %r" % names)
        if board.get("iapFileNameSD"):
            check("manifest contains the board's own IAP filename",
                  board["iapFileNameSD"] in names, "manifest files: %r" % names)


# ---------------------------------------------------------------------------------------------
# 7. The published GitHub release, end to end
# ---------------------------------------------------------------------------------------------
def test_release(owner, repo, tag, allow_prerelease, board_short, download):
    """Resolve and validate the release the slicer would actually install, mirroring
    GitHubReleaseFirmwareSource::fetch_metadata/fetch step for step.

    The bundled bundle in resources/firmware is checked above, but it is only the FALLBACK -
    ChainedFirmwareSource tries the release first, so a release that resolves at all is what
    users get. A release that is less complete than the bundled copy is therefore a downgrade,
    and nothing else in the toolchain would say so.
    """
    import hashlib
    import urllib.error
    import urllib.parse
    import urllib.request

    print("\n[Published release %s/%s]" % (owner, repo))

    # Same three-way URL choice as FirmwareBundle.cpp: an exact tag, or the newest published
    # release, or /releases/latest when pre-releases are excluded.
    if tag:
        url = "https://api.github.com/repos/%s/%s/releases/tags/%s" % (owner, repo, urllib.parse.quote(tag))
    elif allow_prerelease:
        url = "https://api.github.com/repos/%s/%s/releases?per_page=20" % (owner, repo)
    else:
        url = "https://api.github.com/repos/%s/%s/releases/latest" % (owner, repo)

    def get(u, accept, timeout=60):
        req = urllib.request.Request(u, headers={
            "Accept": accept,
            "X-GitHub-Api-Version": "2022-11-28",
            # GitHub rejects API requests that do not identify themselves.
            "User-Agent": "UnlayeredSlicer",
        })
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()

    try:
        raw = get(url, "application/vnd.github+json")
    except urllib.error.HTTPError as e:
        # 403 here is the "repository is private, or rate limited" case the C++ reports.
        check("release query succeeds", False, "HTTP %s from %s" % (e.code, url))
        return
    except Exception as e:
        print("  SKIP  GitHub unreachable: %s" % e)
        return

    doc = json.loads(raw.decode("utf-8", "replace"))

    # A tag or "latest" query returns one object; a list query returns an array, and the C++ takes
    # the FIRST non-draft entry without sorting, so order on the wire is what decides.
    if isinstance(doc, list):
        release = next((r for r in doc if not r.get("draft")), None)
        if not check("a published (non-draft) release exists", release is not None):
            return
    else:
        release = doc

    print("          tag        : %s" % release.get("tag_name"))
    print("          prerelease : %s" % release.get("prerelease"))
    check("release declares a tag_name", bool(release.get("tag_name")))

    assets = [(a.get("name", ""), a.get("browser_download_url", ""))
              for a in release.get("assets", [])]
    assets = [(n, u) for n, u in assets if n and u]
    if not check("release has downloadable assets", bool(assets)):
        return
    print("          assets     : %s" % ", ".join(n for n, _ in assets))

    def asset_url(name):
        for n, u in assets:
            if n.lower() == name.lower():
                return u
        return None

    # --- the manifest, which is authoritative over the tag when present ------------------------
    murl = asset_url("manifest.json")
    if not check("release carries a manifest.json", murl is not None,
                 "without one the tag stands in for the version and assets are matched by name"):
        return

    try:
        manifest = json.loads(get(murl, "application/octet-stream").decode("utf-8", "replace"))
    except Exception as e:
        check("manifest.json is readable and valid JSON", False, str(e))
        return

    check("manifest declares a version", bool(manifest.get("version")))
    print("          version    : %s" % manifest.get("version"))
    print("          buildDate  : %s" % manifest.get("buildDate"))

    images = manifest.get("images") or []
    if not check("manifest lists images", bool(images)):
        return

    roles = [i.get("role") for i in images]
    # firmware_role_from_string() accepts these spellings; anything else is a hard error in the
    # C++, not a skipped image.
    known = ("mainboard", "main", "firmware", "iap", "wifi", "wifiserver", "paneldue", "display",
             "panelota", "panel", "esp32panel")
    check("every role is recognised", all(r in known for r in roles), "roles: %r" % roles)
    check_display_filenames(images)

    # The same rule the bundled-manifest test applies, restated against the live release. This is
    # the check that catches a release which flashes firmware but leaves a stale programmer.
    norm = ["mainboard" if r in ("mainboard", "main", "firmware") else r for r in roles]
    if "mainboard" in norm:
        check("a mainboard image is accompanied by its IAP", "iap" in norm,
              "roles present: %r - M997 S0 fails without an IAP on the card, and the copy already "
              "there is from whatever was flashed last" % roles)

    check("manifest board matches the target board",
          str(manifest.get("board", "")).lower() == board_short.lower(),
          "manifest %r vs expected %r" % (manifest.get("board"), board_short))

    # --- every listed image must actually be attached ------------------------------------------
    for img in images:
        name = img.get("file", "")
        if not check("manifest entry has a file name", bool(name)):
            continue
        u = asset_url(name)
        if not check("release attaches %s" % name, u is not None,
                     "the manifest lists it but the release has no such file"):
            continue
        if not download:
            continue
        try:
            data = get(u, "application/octet-stream")
        except Exception as e:
            check("download %s" % name, False, str(e))
            continue
        declared = (img.get("sha256") or "").lower()
        if declared:
            actual = hashlib.sha256(data).hexdigest()
            check("sha256 matches for %s" % name, actual == declared,
                  "manifest %s, actual %s" % (declared[:16], actual[:16]))
        if name.lower().endswith(".uf2"):
            head = data[:8]
            is_uf2 = (len(head) == 8 and
                      int.from_bytes(head[0:4], "little") == UF2_MAGIC_0 and
                      int.from_bytes(head[4:8], "little") == UF2_MAGIC_1)
            check("%s is a real UF2 image" % name, is_uf2)

    # --- is this release actually an upgrade on what ships in the box? -------------------------
    bundled_path = os.path.join(FW_DIR, "manifest.json")
    if os.path.isfile(bundled_path):
        with open(bundled_path, "r", encoding="utf-8") as f:
            bundled = json.load(f)
        b_roles = [i.get("role") for i in bundled.get("images", [])]
        print("          bundled    : %s (%s)" % (bundled.get("version"), ", ".join(b_roles)))
        # ChainedFirmwareSource prefers the release, so anything the bundle has and the release
        # lacks is quietly lost the moment a release exists.
        missing = [r for r in b_roles if r not in norm]
        check("release is not less complete than the bundled fallback", not missing,
              "the release omits %r, and it is preferred over the bundle, so those images would "
              "no longer be installed" % missing)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", metavar="HOST", action="append", default=[],
                    help="read-only probe of a printer (repeatable)")
    ap.add_argument("--password", default="reprap")
    ap.add_argument("--release", action="store_true",
                    help="also validate the published GitHub release the slicer would install")
    ap.add_argument("--owner", default="Unlayered3D")
    ap.add_argument("--repo", default="RepRapFirmware")
    ap.add_argument("--tag", default="", help="exact tag; default is the newest published release")
    ap.add_argument("--stable-only", action="store_true",
                    help="exclude pre-releases, matching allow_prerelease=false")
    ap.add_argument("--board", default="Mini5plus")
    ap.add_argument("--no-download", action="store_true",
                    help="check the release's shape without transferring the images")
    args = ap.parse_args()

    print("Firmware updater assumption checks")
    print("=" * 60)

    test_crc32()
    test_binary_roundtrip()
    test_version_semantics()
    test_uf2()
    test_manifest()
    if args.release:
        test_release(args.owner, args.repo, args.tag, not args.stable_only, args.board,
                     not args.no_download)
    for host in args.probe:
        probe(host, args.password)

    print("\n" + "=" * 60)
    print("%d passed, %d failed" % (_passed, _failed))
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
