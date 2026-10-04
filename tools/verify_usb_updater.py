#!/usr/bin/env python3
"""Verify the assumptions the USB/serial updater is built on, against a real board.

The network updater talks to a Duet over rr_*. This proves the serial equivalents behave the way
DuetSerialClient will assume, before any C++ exists. Each check corresponds to a decision that
would silently corrupt a firmware image or a config file if the assumption were wrong.

The mapping being proved:

    rr_model   -> M409 K"key" F"flags"
    rr_gcode   -> the line itself
    rr_upload  -> M559 P"<absolute path>" S<size> C<crc32> + exactly <size> raw bytes
    rr_mkdir   -> M470 P"dir"            (P, not the D the Duet docs show)
    rr_filelist-> M20 S3 P"dir"
    rr_download-> M1763 P"path" O<off> L<len>   (Unlayered fork only; absent on stock RRF)

Usage:
    python tools/verify_usb_updater.py                          # offline checks only
    python tools/verify_usb_updater.py --list                   # candidate Duet ports
    python tools/verify_usb_updater.py --port COM15             # + read-only probe
    python tools/verify_usb_updater.py --port COM15 --write     # + writes a scratch file
    python tools/verify_usb_updater.py --port COM15 --write --http <printer-ip>
                                                                # + cross-check against rr_*

--write uploads a scratch file to 0:/sys/, verifies it with M38, reads it back with M1763 if the
board has it, and deletes it. It never touches 0:/firmware and never sends M997.
"""

import argparse
import base64
import json
import os
import random
import re
import sys
import time
import zlib

_passed = 0
_failed = 0
_skipped = 0

SCRATCH_PATH = "0:/sys/unlayered-usbtest.bin"

# M559's default folder is sys/, but MassStorage::CombineName ignores the default when the name
# starts with '/' or a drive spec, so every path we send is absolute and unambiguous.
DRIVE_RE = re.compile(r"^\d:/")


def check(name, ok, detail=""):
    global _passed, _failed
    if ok:
        _passed += 1
        print("  PASS  %s" % name)
    else:
        _failed += 1
        print("  FAIL  %s%s" % (name, ("\n          " + detail) if detail else ""))
    return ok


def skip(name, why):
    global _skipped
    _skipped += 1
    print("  SKIP  %s\n          %s" % (name, why))


class RrfSerialError(Exception):
    pass


class RrfSerial:
    """A minimal, deliberately synchronous RRF serial channel.

    Both USB channels run Compatibility::Marlin, so every command is acknowledged with a bare
    "ok" line - that is the framing this relies on. The one place it does NOT hold is the body of
    an M559 upload, where the firmware sends nothing at all until the byte count is reached.
    """

    def __init__(self, port, baud=115200, verbose=False):
        import serial  # pyserial; imported here so --list/offline work without a board

        self.verbose = verbose
        # No hardware flow control - RRF does not use any on USB; back-pressure is the CDC
        # endpoint NAKing when its 256-byte input ring is full.
        self.ser = serial.Serial(port, baud, timeout=0.2, write_timeout=20,
                                 dsrdtr=False, rtscts=False)
        # DTR must be ASSERTED or RRF ignores the channel entirely - it gates on the CDC line
        # state, so a port opened with DTR low looks alive and answers nothing. RTS is left alone:
        # it means nothing to a native-USB board and toggling it resets anything that has an
        # auto-reset circuit.
        self.ser.dtr = True
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self._buf = b""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _read_line(self, deadline):
        """One line, or None once the deadline passes."""
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                return line.decode("utf-8", "replace").rstrip("\r")
            if time.time() >= deadline:
                return None
            chunk = self.ser.read(512)
            if chunk:
                self._buf += chunk

    def command(self, cmd, timeout=10.0):
        """Send one line, collect everything up to its 'ok'. Returns the reply without the ok.

        Unsolicited output (a status line, a warning from something else) is collected too rather
        than being treated as an error - an RRF channel emits it at any time.
        """
        if self.verbose:
            print("          >> %s" % cmd)
        self.ser.write((cmd + "\n").encode("ascii"))
        self.ser.flush()
        return self._collect_until_ok(timeout, what=cmd)

    def _collect_until_ok(self, timeout, what):
        deadline = time.time() + timeout
        lines = []
        while True:
            line = self._read_line(deadline)
            if line is None:
                raise RrfSerialError("timed out after %.1fs waiting for 'ok' to %r; got %r"
                                     % (timeout, what, "\n".join(lines)))
            if self.verbose:
                print("          << %s" % line)
            if line == "ok" or line.startswith("ok "):
                # "ok <reply>" is the M105/M998 shape; keep whatever rode along with it.
                rest = line[3:].strip()
                if rest:
                    lines.append(rest)
                return "\n".join(lines).strip()
            if line:
                lines.append(line)

    def model(self, key, flags="v", timeout=10.0):
        """M409 is rr_model. Keep keys narrow: a wide one can outgrow the output buffers."""
        reply = self.command('M409 K"%s" F"%s"' % (key, flags), timeout)
        start = reply.find("{")
        if start < 0:
            raise RrfSerialError("M409 %s returned no JSON: %r" % (key, reply))
        return json.loads(reply[start:]).get("result")

    def upload(self, remote_path, data, progress=None, chunk=4096):
        """rr_upload's equivalent: M559 with an exact size and a CRC32 the firmware rechecks.

        The order here is the safety-critical part. The body must not be sent until the firmware
        has confirmed the file is open, because if the open failed the channel is still parsing
        G-code and every byte of a firmware image would be interpreted as commands.
        """
        if not DRIVE_RE.match(remote_path) and not remote_path.startswith("/"):
            raise RrfSerialError("remote path must be absolute, got %r" % remote_path)

        crc = zlib.crc32(data) & 0xFFFFFFFF
        reply = self.command('M559 P"%s" S%d C%d' % (remote_path, len(data), crc), timeout=15.0)
        if "Writing to file" not in reply:
            raise RrfSerialError("M559 did not open the file, refusing to send the body: %r" % reply)

        sent = 0
        for off in range(0, len(data), chunk):
            self.ser.write(data[off:off + chunk])
            sent += len(data[off:off + chunk])
            if progress:
                progress(sent, len(data))

        # One trailing newline, ALWAYS. Measured on the MK3 2026-09-13: without it roughly 40% of
        # uploads never complete - the firmware sits in binary mode for ever and the next thing the
        # host sends is eaten as file data, which is unrecoverable without a reset.
        #
        # Nothing is lost when this happens. The final short USB packet simply is not visible to
        # the firmware until more traffic arrives on the endpoint, so one more byte makes the whole
        # tail appear at once. Proof that it is delivery and not loss: when a stalled upload is
        # nudged this way it completes with "Done saving file." and a CRC the firmware agrees with,
        # and the newline itself never lands in the file - WriteBinaryToFile reaches the declared
        # byte count first and leaves the newline in the ring, where it parses as a blank line and
        # is discarded.
        #
        # It is harmless in the normal case for the same reason: a blank line produces no command
        # and no reply. And if a byte ever genuinely were lost, the newline would land in the file
        # instead and the CRC check would fail - which is a loud, correct answer rather than a hang.
        self.ser.write(b"\n")
        self.ser.flush()

        # Nothing comes back mid-body; the firmware replies only once the byte count is reached.
        # Allow generously for the SD write of a large image.
        reply = self._collect_until_ok(timeout=120.0, what="end of M559 body")
        if "CRC32 checksum doesn't match" in reply:
            raise RrfSerialError("firmware rejected the upload: CRC32 mismatch")
        if "Done saving file." not in reply:
            raise RrfSerialError("unexpected reply at end of upload: %r" % reply)
        return crc

    def file_crc32(self, remote_path):
        """M38 - documented as SHA1, actually CRC-32, 8 lowercase hex digits."""
        reply = self.command("M38 %s" % remote_path, timeout=120.0)
        m = re.search(r"\b([0-9a-f]{8})\b", reply)
        if not m:
            raise RrfSerialError("M38 %s: %r" % (remote_path, reply))
        return int(m.group(1), 16)

    def read_file(self, remote_path, chunk=512, limit=None):
        """M1763 - the fork's file read. Raises RrfSerialError('unsupported') on stock RRF."""
        out = b""
        offset = 0
        while True:
            reply = self.command('M1763 P"%s" O%d L%d' % (remote_path, offset, chunk), timeout=15.0)
            if "not supported" in reply.lower():
                raise RrfSerialError("unsupported")
            start = reply.find("{")
            if start < 0:
                raise RrfSerialError("M1763 returned no JSON: %r" % reply)
            fr = json.loads(reply[start:]).get("fread", {})
            out += base64.b64decode(fr.get("data", ""))
            if fr.get("eof") or not fr.get("len"):
                return out
            offset += fr["len"]
            if limit is not None and len(out) >= limit:
                return out


# ---------------------------------------------------------------------------------------------
# Offline checks
# ---------------------------------------------------------------------------------------------
def test_offline():
    """The data-shape assumptions, provable without a board."""
    print("\n[Offline]")

    # M559's C parameter is rechecked by RRF's own CRC32 class: poly 0xEDB88320, init 0xFFFFFFFF,
    # final XOR - the zlib variant, and the same one mz_crc32 and M38 use. If this were a different
    # variant every upload would be rejected.
    check("CRC-32 of \"123456789\" is 0xCBF43926",
          zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926)

    # M559 takes C as a decimal unsigned; a value above 2^31 must not come out negative.
    big = zlib.crc32(b"\xff" * 64) & 0xFFFFFFFF
    check("CRC-32 formats unsigned", ("C%d" % big).startswith("C") and big >= 0,
          "got C%d" % big)

    # An absolute path must bypass M559's default sys/ folder, so paths are always sent absolute.
    check("firmware path is recognised as absolute",
          bool(DRIVE_RE.match("0:/firmware/Duet3Firmware_Mini5plus.uf2")))
    check("a bare name is not", not DRIVE_RE.match("config.g"))

    # M1763's payload is base64 so the reply survives the ordinary G-code reply path.
    blob = bytes(range(256))
    check("base64 round-trips every byte value",
          base64.b64decode(base64.b64encode(blob)) == blob)


# ---------------------------------------------------------------------------------------------
# Live, read-only
# ---------------------------------------------------------------------------------------------
def probe(rrf):
    """Exactly the read-only calls the updater makes before it would write anything."""
    print("\n[Live probe - read only]")

    board = rrf.model("boards[0]", "v")
    ok = check("M409 boards[0] readable", isinstance(board, dict))
    if not ok:
        return None

    # The same four fields DuetRrfClient::read_board_info parses. A missing one would mean
    # uploading firmware under the wrong name.
    for field in ("firmwareVersion", "firmwareFileName", "iapFileNameSD", "shortName"):
        check("boards[0].%s present" % field,
              board.get(field) not in (None, ""), "got %r" % board.get(field))

    print("          shortName       : %s" % board.get("shortName"))
    print("          firmwareVersion : %s" % board.get("firmwareVersion"))
    print("          firmwareDate    : %s" % board.get("firmwareDate"))
    print("          firmwareFileName: %s" % board.get("firmwareFileName"))
    print("          iapFileNameSD   : %s" % board.get("iapFileNameSD"))

    state = rrf.model("state", "f")
    status = (state or {}).get("status")
    check("state.status present", bool(status), "got %r" % status)
    print("          status          : %s" % status)
    if status and status not in ("idle", "off"):
        print("          NOTE: not idle - the updater would refuse to write in this state")

    network = rrf.model("network", "v")
    if isinstance(network, dict):
        print("          network.name    : %s" % network.get("name"))

    # M470 with no P must complain about P, not about D. This is the cheapest possible proof that
    # the parameter letter in the plan is right, and it writes nothing.
    reply = rrf.command("M470")
    check("M470 wants P, not D", "'P'" in reply, "got %r" % reply)

    # M20 S3 is rr_filelist.
    reply = rrf.command('M20 S3 P"0:/sys"', timeout=20.0)
    start = reply.find("{")
    listing = json.loads(reply[start:]) if start >= 0 else {}
    check("M20 S3 returns a JSON listing with files[]",
          isinstance(listing.get("files"), list),
          "got %r" % reply[:200])
    if isinstance(listing.get("files"), list):
        print("          0:/sys holds %d entries" % len(listing["files"]))

    # M38 on a file that exists on every card.
    try:
        crc = rrf.file_crc32("0:/sys/config.g")
        check("M38 0:/sys/config.g returns a CRC-32", True)
        print("          config.g CRC-32 : %08x" % crc)
    except RrfSerialError as e:
        check("M38 0:/sys/config.g returns a CRC-32", False, str(e))
        crc = None

    # M1763 is the fork's addition; a stock or older board answers "not supported", which is a
    # skip rather than a failure - it only costs backups, not the ability to update.
    try:
        data = rrf.read_file("0:/sys/config.g", limit=1024)
        check("M1763 reads config.g back", len(data) > 0, "read %d bytes" % len(data))
        if crc is not None:
            full = rrf.read_file("0:/sys/config.g")
            check("M1763 read-back CRC matches M38",
                  zlib.crc32(full) & 0xFFFFFFFF == crc,
                  "read %d bytes, crc %08x, M38 said %08x"
                  % (len(full), zlib.crc32(full) & 0xFFFFFFFF, crc))
    except RrfSerialError as e:
        if "unsupported" in str(e):
            skip("M1763 file read", "this firmware has no M1763 - backups over USB are unavailable "
                                    "until it is updated")
        else:
            check("M1763 reads config.g back", False, str(e))

    return board


# ---------------------------------------------------------------------------------------------
# Live, writing
# ---------------------------------------------------------------------------------------------
def write_pass(rrf, http_host=None):
    """Upload a scratch file, verify it, read it back, delete it. Never touches 0:/firmware."""
    print("\n[Live write pass - %s]" % SCRATCH_PATH)

    # Deliberately nasty content: every byte value, an embedded "M29" and "M112" (the two
    # sequences that would end or hijack a text-mode M28 upload), quotes, semicolons and CRLFs.
    # None of it may matter, because M559's body never reaches the G-code parser.
    rnd = random.Random(20260913)
    payload = bytearray()
    payload += bytes(range(256)) * 4
    payload += b"\r\nM29\r\n; comment \" quote\r\nM112\n"
    payload += bytes(rnd.randrange(256) for _ in range(4096))
    payload += b"<!-- **EoF** -->"          # the sentinel, which S<size> must make inert
    payload = bytes(payload)

    t0 = time.time()
    try:
        crc = rrf.upload(SCRATCH_PATH, payload)
    except RrfSerialError as e:
        check("M559 upload of %d bytes" % len(payload), False, str(e))
        return
    dt = time.time() - t0
    check("M559 upload of %d bytes" % len(payload), True)
    print("          %.1f s, %.1f kB/s" % (dt, len(payload) / 1024.0 / max(dt, 1e-6)))

    try:
        on_card = rrf.file_crc32(SCRATCH_PATH)
        check("M38 CRC matches what we sent", on_card == crc,
              "card %08x, sent %08x" % (on_card, crc))
    except RrfSerialError as e:
        check("M38 CRC matches what we sent", False, str(e))

    # The EOF sentinel must have been written as data, not have truncated the file: the CRC above
    # already proves it, but the length is the clearer statement of intent.
    reply = rrf.command('M20 S3 P"0:/sys"', timeout=20.0)
    start = reply.find("{")
    if start >= 0:
        for f in json.loads(reply[start:]).get("files", []):
            if f.get("name") == os.path.basename(SCRATCH_PATH):
                check("file is the exact size we declared", f.get("size") == len(payload),
                      "card says %r, sent %d" % (f.get("size"), len(payload)))

    try:
        back = rrf.read_file(SCRATCH_PATH)
        check("M1763 read-back is byte-identical", back == payload,
              "read %d bytes of %d" % (len(back), len(payload)))
    except RrfSerialError as e:
        if "unsupported" in str(e):
            skip("M1763 read-back", "no M1763 on this firmware")
        else:
            check("M1763 read-back is byte-identical", False, str(e))

    if http_host:
        cross_check_http(http_host, payload)

    reply = rrf.command("M30 %s" % SCRATCH_PATH, timeout=15.0)
    check("M30 removes the scratch file", "rror" not in reply, "got %r" % reply)


def cross_check_http(host, payload):
    """The same bytes, fetched back over rr_download. Proves the two transports agree."""
    import urllib.request
    import urllib.parse
    import datetime

    print("\n[Cross-check against rr_download on %s]" % host)
    base = "http://%s/" % host
    ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    try:
        with urllib.request.urlopen(
                base + "rr_connect?password=reprap&time=" + urllib.parse.quote(ts), timeout=6) as r:
            r.read()
        time.sleep(1.2)
        url = base + "rr_download?name=" + urllib.parse.quote(SCRATCH_PATH, safe="")
        with urllib.request.urlopen(url, timeout=20) as r:
            got = r.read()
        check("rr_download returns exactly what M559 wrote", got == payload,
              "http gave %d bytes, we sent %d" % (len(got), len(payload)))
        time.sleep(1.2)
        urllib.request.urlopen(base + "rr_disconnect", timeout=6).read()
    except Exception as e:
        skip("rr_download cross-check", "%s: %s" % (type(e).__name__, e))


# ---------------------------------------------------------------------------------------------
def list_ports():
    """Candidate boards. RRF reports an empty USB serial number, so VID/PID and the port's
    location are the only identity available - which is also why a reconnect after a firmware
    update has to match on location."""
    try:
        import serial.tools.list_ports as lp
    except ImportError:
        print("pyserial is not installed for %s" % sys.executable)
        return 1
    ports = list(lp.comports())
    if not ports:
        print("no serial ports")
        return 1
    for p in ports:
        duet = (p.vid == 0x1D50)
        print("%-8s vid:pid=%04x:%04x %-38s location=%s%s"
              % (p.device, p.vid or 0, p.pid or 0, p.description, p.location,
                 "   <- Duet" if duet else ""))
        if duet and p.pid == 0x60EF:
            print("         PID 60EF is the dual-CDC composite: RRF 3.7, two G-code channels.")
            print("         Use the LOWER-numbered of the pair - that is the plain text console.")
        elif duet and p.pid == 0x60EE:
            print("         PID 60EE is the single-CDC device: RRF 3.6 and earlier")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM15")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by a native-USB CDC device; kept for non-USB links")
    ap.add_argument("--list", action="store_true", help="list candidate Duet ports and exit")
    ap.add_argument("--write", action="store_true",
                    help="also run the write pass (scratch file in 0:/sys, deleted afterwards)")
    ap.add_argument("--http", metavar="HOST",
                    help="cross-check the written bytes against rr_download on this host")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo the wire traffic")
    args = ap.parse_args()

    if args.list:
        return list_ports()

    test_offline()

    if args.port:
        try:
            rrf = RrfSerial(args.port, args.baud, args.verbose)
        except Exception as e:
            check("open %s" % args.port, False, "%s: %s" % (type(e).__name__, e))
            rrf = None
        if rrf:
            try:
                board = probe(rrf)
                if args.write:
                    status = (rrf.model("state", "f") or {}).get("status")
                    if status not in ("idle", "off"):
                        skip("write pass", "printer is %r, not idle" % status)
                    else:
                        write_pass(rrf, args.http)
            except RrfSerialError as e:
                check("serial exchange", False, str(e))
            finally:
                rrf.close()
    else:
        skip("live checks", "pass --port COMnn to talk to a board (--list to find one)")

    print("\n%d passed, %d failed, %d skipped" % (_passed, _failed, _skipped))
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
