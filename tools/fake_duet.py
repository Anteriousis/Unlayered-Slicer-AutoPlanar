#!/usr/bin/env python3
"""A fake Duet (RepRapFirmware standalone HTTP API) with its SD card in a folder, for testing the
slicer's Config and Firmware updaters without a printer.

  python tools/fake_duet.py --card <folder> [--port 8099] [--firmware 3.7.0-beta.1+unlayered.19]
                            [--name "Unlayered Test Printer"] [--status idle] [--log requests.log]
                            [--bad-feed-zip sys.zip]

The folder is the card: 0:/sys/homez.g is <folder>/sys/homez.g. Implements what DuetRrfClient uses,
with RRF's answers, including the ones the updater depends on:

  rr_connect      {"err":0,"sessionKey":N}; refused (HTTP 503) for 3 s after M999, like a reboot
  rr_disconnect   {"err":0}
  rr_model        key=boards[0] | network | state, flags ignored; {"key":..,"flags":..,"result":{..}}
  rr_gcode        {"buff":N,"err":0}; M999 starts the fake reboot
  rr_upload       POST; Content-Length REQUIRED; crc32 query checked (err=1 on mismatch, file removed)
  rr_download     the bytes, or HTTP 404 for a missing file
  rr_filelist     {"dir","first","files":[{type,name,size,date}],"next":0}; err=2 for a missing dir
  rr_mkdir        err=0 when created, err=1 when it already exists (RRF cannot tell them apart)
  rr_delete       err=0, or err=1 for a missing file

--bad-feed-zip also serves a one-release configuration feed at /badfeed/ whose sys.zip does NOT match
its advertised digest, to prove the slicer refuses such a download.

Every request is appended to --log as "METHOD path?query -> status", so a test can show exactly what
the slicer asked of the board. Nothing here ever talks to a real printer.
"""

import argparse
import hashlib
import json
import os
import threading
import time
import urllib.parse
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"reboot_until": 0.0, "session": 1000, "reboots": 0}
LOCK = threading.Lock()


def card_path(root, name):
    """0:/sys/x.g -> <root>/sys/x.g, refusing anything that leaves the card."""
    if name.startswith("0:"):
        name = name[2:]
    rel = os.path.normpath(name.lstrip("/\\"))
    if rel.startswith("..") or os.path.isabs(rel):
        raise ValueError("path leaves the card: %s" % name)
    return os.path.join(root, rel)


def make_handler(args):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"  # RRF closes every connection; so do we

        def log_message(self, fmt, *a):
            pass

        def note(self, status):
            if args.log:
                with LOCK, open(args.log, "a", encoding="utf-8") as f:
                    f.write("%s %s %s -> %d\n" % (time.strftime("%H:%M:%S"), self.command, self.path, status))

        def send(self, status, body, ctype="application/json"):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.note(status)

        def rebooting(self):
            return time.time() < STATE["reboot_until"]

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True))
            p = url.path

            if p.startswith("/badfeed/"):
                return self.bad_feed(p)
            if self.rebooting():
                return self.send(503, b"rebooting", "text/plain")

            if p == "/rr_connect":
                if q.get("password", "reprap") not in ("", "reprap"):
                    return self.send(200, {"err": 1})
                with LOCK:
                    STATE["session"] += 1
                    key = STATE["session"]
                return self.send(200, {"err": 0, "sessionTimeout": 8000, "boardType": "duet3mini5plus",
                                       "apiLevel": 2, "sessionKey": key})
            if p == "/rr_disconnect":
                return self.send(200, {"err": 0})
            if p == "/rr_model":
                key = q.get("key", "")
                if key == "boards[0]":
                    result = {"firmwareVersion": args.firmware, "firmwareDate": "2026-10-08",
                              "firmwareName": "RepRapFirmware for Duet 3 Mini 5+",
                              "firmwareFileName": "Duet3Firmware_Mini5plus.uf2",
                              "iapFileNameSD": "Duet3_SDiap32_Mini5plus.bin",
                              "wifiFirmwareFileName": "DuetWiFiServer.bin", "shortName": "Mini5plus",
                              "name": "Duet 3 Mini 5+", "uniqueId": "FAKE0-DUET0-19TEST"}
                elif key == "network":
                    result = {"name": args.name, "interfaces": [{"type": "ethernet"}]}
                elif key == "state":
                    result = {"status": args.status}
                else:
                    result = {}
                return self.send(200, {"key": key, "flags": q.get("flags", ""), "result": result})
            if p == "/rr_gcode":
                code = q.get("gcode", "").strip()
                if code.upper().startswith("M999"):
                    with LOCK:
                        STATE["reboot_until"] = time.time() + 3.0
                        STATE["reboots"] += 1
                return self.send(200, {"buff": 255, "err": 0})
            if p == "/rr_download":
                try:
                    f = card_path(args.card, q.get("name", ""))
                except ValueError:
                    return self.send(404, {"err": 1})
                if not os.path.isfile(f):
                    return self.send(404, b"", "text/plain")
                with open(f, "rb") as fh:
                    return self.send(200, fh.read(), "application/octet-stream")
            if p == "/rr_filelist":
                try:
                    d = card_path(args.card, q.get("dir", ""))
                except ValueError:
                    return self.send(200, {"err": 2})
                if not os.path.isdir(d):
                    return self.send(200, {"err": 2})
                files = []
                for n in sorted(os.listdir(d)):
                    full = os.path.join(d, n)
                    files.append({"type": "d" if os.path.isdir(full) else "f", "name": n,
                                  "size": 0 if os.path.isdir(full) else os.path.getsize(full),
                                  "date": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(os.path.getmtime(full)))})
                return self.send(200, {"dir": q.get("dir"), "first": 0, "files": files, "next": 0})
            if p == "/rr_mkdir":
                try:
                    d = card_path(args.card, q.get("dir", ""))
                except ValueError:
                    return self.send(200, {"err": 1})
                if os.path.isdir(d):
                    return self.send(200, {"err": 1})
                try:
                    os.mkdir(d)  # one level at a time, like FatFs
                except OSError:
                    return self.send(200, {"err": 1})
                return self.send(200, {"err": 0})
            if p == "/rr_delete":
                try:
                    f = card_path(args.card, q.get("name", ""))
                except ValueError:
                    return self.send(200, {"err": 1})
                if os.path.isfile(f):
                    os.remove(f)
                    return self.send(200, {"err": 0})
                return self.send(200, {"err": 1})
            return self.send(404, {"err": 1})

        def do_POST(self):
            url = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True))
            if self.rebooting():
                return self.send(503, b"rebooting", "text/plain")
            if url.path != "/rr_upload":
                return self.send(404, {"err": 1})
            length = self.headers.get("Content-Length")
            if length is None:
                return self.send(200, {"err": 1})  # RRF refuses an upload with no Content-Length
            data = self.rfile.read(int(length))
            try:
                f = card_path(args.card, q.get("name", ""))
            except ValueError:
                return self.send(200, {"err": 1})
            if not os.path.isdir(os.path.dirname(f)):
                return self.send(200, {"err": 1})  # rr_upload cannot create the parent directory
            with open(f, "wb") as fh:
                fh.write(data)
            want = q.get("crc32")
            if want is not None and int(want, 16) != (zlib.crc32(data) & 0xFFFFFFFF):
                os.remove(f)
                return self.send(200, {"err": 1})
            return self.send(200, {"err": 0})

        def bad_feed(self, p):
            # /badfeed/repos/<owner>/printer-config/releases[?...] and /badfeed/assets/sys.zip
            if not args.bad_feed_zip:
                return self.send(404, {"error": "no bad feed configured"})
            host = self.headers.get("Host", "127.0.0.1:%d" % args.port)
            data = open(args.bad_feed_zip, "rb").read()
            if p.endswith("/assets/sys.zip"):
                return self.send(200, data, "application/octet-stream")
            wrong = hashlib.sha256(data + b"tampered").hexdigest()
            release = {"tag_name": "config-bad", "name": "bad", "body": "", "draft": False, "prerelease": False,
                       "assets": [{"name": "sys.zip", "size": len(data), "digest": "sha256:" + wrong,
                                   "browser_download_url": "http://%s/badfeed/assets/sys.zip" % host}]}
            return self.send(200, [release] if p.endswith("/releases") else release)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", required=True)
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--firmware", default="3.7.0-beta.1+unlayered.19")
    ap.add_argument("--name", default="Unlayered Test Printer")
    ap.add_argument("--status", default="idle")
    ap.add_argument("--log")
    ap.add_argument("--bad-feed-zip")
    args = ap.parse_args()
    os.makedirs(args.card, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(args))
    print("fake Duet on http://127.0.0.1:%d, card %s" % (args.port, os.path.abspath(args.card)), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
