# Updating a printer over USB

The **Firmware** and **Config** buttons in the physical printer dialog can push to a Duet over its
USB serial port as well as over the network. This is what makes a printer with no network — a
machine on the bench, a customer with no Ethernet run — updatable at all.

Everything here was measured against a Duet 3 Mini 5+ on 2026-09-13, including a 1,926,656-byte
firmware image in **14.1 s at 134 kB/s** with the CRC checked by the board.

---

## 1. The wire protocol

There is no new transfer protocol. Every rr_* call has a G-code equivalent, and they are
byte-identical in RRF 3.6.2 and 3.7 unless noted:

| HTTP | Serial | Notes |
|---|---|---|
| `rr_model` | `M409 K"key" F"flags"` | |
| `rr_gcode` | the line itself | |
| `rr_upload` | `M559 P"<abs path>" S<size> C<crc32>` + raw bytes | see below |
| `rr_mkdir` | `M470 P"dir"` | **`P`, not the `D` the Duet docs show** |
| `rr_filelist` | `M20 S3 P"dir" R<first>` | identical document to `rr_filelist` |
| `rr_delete` | `M30 <path>` | unprecedented string, no quotes |
| `rr_download` | `M1763 P"path" O<off> L<len>` | **this fork only** — see §4 |
| connect / session / password | — | not needed; USB is physical access |

`M38 <path>` verifies any write. Despite its name, and the Duet documentation calling it SHA1, it
returns a **CRC-32** as eight lowercase hex digits — the same variant `compute_crc32()` produces.

Both USB channels run `Compatibility::Marlin`, so every command is terminated by `ok`. Nothing at
all is sent during an M559 body.

### Why M559 and not M28

`M28` writes the *parsed* line, not the bytes. It strips inline comments and N-words, drops blank
and bad-checksum lines silently, and **truncates any line over 255 characters with no error**,
because the length check is never reached on the write path. `M559` sets `binaryWrite`, which makes
`GCodeInput.cpp` bypass the G-code parser entirely: 8-bit clean, with the board rechecking the
CRC at close. Use it for text files too — one code path, verified end to end.

---

## 2. Two rules that are not optional

**Wait for the `Writing to file:` ack before sending a single byte of the body.** If the open
failed — a full card, a missing directory, a line corrupted in transit — the channel is still
parsing G-code, and a firmware image streamed into it is executed as commands on a machine with
heaters and motors. This is the most dangerous failure mode in the feature and the ordering is the
whole mitigation.

**Always send one trailing newline after the body.** Without it roughly **40% of uploads never
complete** (measured: 8 stalls in 20 without, 0 in 30 with). The board sits in binary mode for ever
and the next thing the host sends is eaten as file data.

Nothing is lost when this happens. The final short USB packet is not visible to the firmware until
more traffic arrives on the endpoint, so one more byte makes the whole tail appear at once. The
proof it is delivery and not loss: a nudged upload completes with `Done saving file.` and a CRC the
*firmware itself* agrees with, and the newline never lands in the file — the byte count is reached
first and the newline stays in the ring, where a blank line produces no command and no reply.

> A day was lost chasing this as a `FileStore::Length()` off-by-one in the firmware. It is not, and
> that firmware change was reverted. The fork's diff for this feature is `M1763` and nothing else.

**There is no abort.** `Init()` and `Reset()` both leave `binaryWriting` set, and closing the port
does not clear it. Cancelling therefore means *finishing* the byte count with padding, letting the
CRC check fail, and deleting the file — never closing the port mid-payload.

---

## 3. USB identity is not stable across a firmware update

| | RRF 3.6 | RRF 3.7 |
|---|---|---|
| Product id | `1D50:60EE` | `1D50:60EF` |
| Shape | single CDC | IAD composite, **two** CDC ports |
| Example ports | `COM15` | `COM3` (MI_00) **and** `COM4` (MI_02) |

RRF reports an **empty USB serial number**, and the port's own device-instance id changes too
(a non-composite device becomes an interface child of a composite one). What survives is the
**physical socket**: `SerialPortInfo::topology_key`, the location path with the `#USBMI(n)` suffix
stripped. That is what `wait_for_reboot()` re-finds the board by — not the product id, not the COM
number, not the instance id.

Both CDC ports on a 3.7 board are full, independent G-code channels; either works, and the lower
interface is preferred. **Never send `M576.1`** — it switches a USB channel into the SBC binary
protocol.

### A 3.6 board may be unopenable on a PC that has the old Duet driver

Duet3D's 2019 INF (`oem218.inf`, "Duet 3 motion control electronics") matches product id `60EE` and
binds `usbser` at the device level, which fails on Windows 11 with **Code 10**
(`STATUS_INVALID_PARAMETER`). Product id `60EF` does not match that INF, so a 3.7 board gets
`usbccgp` plus Microsoft's in-box driver and simply works — upgrading a board can fix its own USB.

The port still enumerates and still has a name in this state, so it looks exactly like a healthy
printer that will not answer. `SerialPortInfo::problem_code` (from `DEVPKEY_Device_ProblemCode`)
is read during the scan so the picker can say *why*. Remedy on the PC: Device Manager ▸ Update
driver ▸ *USB Serial Device*, or `pnputil /delete-driver oem218.inf /uninstall`.

**DTR must be asserted** or RRF ignores the channel entirely — it gates on the CDC line state, so
the port opens, looks alive, and answers nothing. On Windows, open `\\.\COMn`: `CreateFile` will not
resolve COM10 and above without the prefix.

---

## 4. `M1763` — reading a file back

The one thing with no stock equivalent. `M503` dumps only `config.g`, and `M36.2` — the only stock
file read — is unusable for backups: it is line-based (CRLF normalised to LF, binary-unsafe) and its
`data` field is **not JSON-escaped**, so any file containing a quoted string, `M550 P"MK3"` for
instance, produces malformed JSON.

So the fork adds, next to its existing `M1750` and `M1760`–`M1762`:

```
M1763 P"0:/sys/config.g" O<offset> L<len>
  -> {"fread":{"path":"...","offset":0,"size":10240,"len":512,"eof":false,"data":"<base64>"}}
```

Unlike `M1760`–`M1762` it is deliberately **not** restricted to an aux port. A missing file is
reported the way `M38` reports one (`Cannot open file: ...`), which is how a caller distinguishes
"nothing to back up" from "the read failed".

Without it a printer can still be updated — only backups and the installed-config readout are
unavailable, and the Config dialog disables its backup checkbox with that reason shown rather than
silently writing a copy that differs from the card.

---

## 5. Code layout

| Piece | Where |
|---|---|
| Transport interface | `src/slic3r/Utils/DuetClient.{hpp,cpp}` |
| HTTP transport | `src/slic3r/Utils/DuetRrfClient.{hpp,cpp}` |
| USB transport | `src/slic3r/Utils/DuetSerialClient.{hpp,cpp}` |
| Port enumeration | `src/slic3r/Utils/Serial.{hpp,cpp}` — `scan_duet_ports()` |
| Connection picker | `src/slic3r/GUI/DuetConnectionPicker.{hpp,cpp}` |
| Protocol harness | `tools/verify_usb_updater.py` |

The bundle sources (`FirmwareBundle.*`, `SysFilesBundle.*`) are untouched and transport-agnostic:
they hand over verified byte buffers and know nothing about how those reach the printer.
`DuetConnectionPicker::make_client_factory()` is the single place in the application that chooses
between HTTP and USB.

### The harness

```
python tools/verify_usb_updater.py --list                    # candidate Duet ports
python tools/verify_usb_updater.py --port COM3               # read-only probe
python tools/verify_usb_updater.py --port COM3 --write --http <printer-ip>
```

The last form writes a scratch file over USB and then fetches it back over Ethernet with
`rr_download`, proving both transports produce identical bytes. Run it before believing any change
to `DuetSerialClient`.

---

## 6. Where the files come from

Unchanged by any of this: both feeds are fetched over HTTP regardless of how the printer is reached,
from `https://unlayered3d.com/printer-updates-api` by default (`github_api_base` overrides it), which
serves GitHub-shaped release JSON. Firmware is public; config needs the customer's Unlayered account
device token, sent as `Authorization: Bearer` and only to that site.
