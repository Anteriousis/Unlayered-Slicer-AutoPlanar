# Unlayered Slicer Web App Bridge — protocol v1

Unlayered Slicer can host a web application as a top-level tab and let that application drive a
small, well-defined set of slicer actions. This document is the complete specification of that
interface.

The tab is a browser control pointed at a URL the user configures. It is the same mechanism the
**Device** tab already uses to embed a printer's own web interface — Duet Web Control, Fluidd,
Mainsail — none of which are part of this program.

**Any web application at any address may use this interface.** There is no vendor allow-list, no
registration, no key. If you can serve a page, you can write a panel. A working reference client
ships in [`resources/web/webapp-example/`](resources/web/webapp-example/) under the MIT licence;
start from it.

---

## 1. Relationship between the two programs

This matters enough to state plainly, because it constrains the design of everything below.

Unlayered Slicer is licensed under the **GNU Affero General Public License, version 3**. A web
application that talks to it across this interface is a **separate program**: it runs in its own
process (usually on its own machine), it is written in whatever language its author likes, it is
licensed however its author likes, and it exchanges nothing with the slicer but serialized JSON
over a documented protocol. Loading a page in a browser control has never made that page a
derivative of the browser, and nothing here changes that.

Two consequences follow, and the interface is shaped by them:

- **The bridge exposes capabilities, and — behind an explicit per-tab opt-in — a typed, filtered
  view of print settings.** You will not find a way to dump a `DynamicPrintConfig`, walk layer
  data, or read a preset's print-host credentials or custom G-code. Settings cross as the `.ini`
  serialisation of a *named* key, one you asked for, under three deny lists no configuration
  widens (§4.1). Those are the slicer's internal data structures; an interface that shovelled them
  to a client would be a much weaker claim to being two programs, quite apart from being a worse
  interface.
- **The bridge itself names no particular application.** The URL, the tab title and every
  capability gate are per-tab configuration. The Agent and Universe tabs carry Unlayered's
  addresses as their shipped defaults, and the generic tab ships with none (§7).

If you host a web application that runs Unlayered Slicer **on your server** to produce output for
remote users, note that AGPL-3.0 §13 asks you to offer those users the Corresponding Source of the
slicer. `slicer.getInfo` returns `app.sourceUrl` precisely so your page can render that offer
without hard-coding it. That obligation covers the slicer, not your application.

---

## 2. Transport

| Direction | Mechanism |
|---|---|
| page → slicer | `window.wx.postMessage(jsonString)` |
| slicer → page | `window.__slicerBridge._recv(obj)`, falling back to `HandleStudio(obj)`, then `window.postMessage(obj, window.location.origin)` |

`window.wx` is installed by the browser control itself and is shared with other parts of the
slicer. **Always post a string**, never an object: WebView2 accepts both, but WKWebView and
WebKitGTK are string-oriented.

You do not normally touch either of these directly. On a trusted origin the slicer injects a small
shim, `window.__slicerBridge`, after each page load:

```js
window.__slicerBridge = Object.freeze({
  version: 1,
  token:   "…64 hex characters…",
  send:    function (method, params) { /* → Promise */ },
  on:      function (eventName, fn) {},
  off:     function (eventName, fn) {},
  _recv:   function (payload) {}          // internal
});
```

`send()` resolves with the response `data`, or rejects with `{code, message, method, data}` — the
response's `data` rides along on a rejection too, which is how a `1013` hands you the list of dirty
keys (§4.1). It rejects with `code: 'no-bridge'` immediately when not running inside the slicer,
and `code: 'timeout'` after 60 s.

A `slicerbridgeready` event fires on `window` once the shim is installed.

### Detecting the slicer

Feature-detect, in this order:

1. `window.__slicerBridge` exists → you are on a trusted origin inside the slicer.
2. `typeof window.wx?.postMessage === 'function'` → you are inside *a* slicer web view, but not
   necessarily on a trusted origin. A `slicer.getInfo` will tell you.
3. Neither → you are in an ordinary browser. Behave accordingly.

**Do not parse the user agent.** It currently contains a `BBL-Slicer/` token inherited from
upstream, it does not identify this fork, and it is rewritten with a macOS platform string on every
platform when the theme changes. It is not a reliable signal for anything.

---

## 3. Envelope

### Request (page → slicer)

```json
{
  "v": 1,
  "id": "wa-7",
  "method": "slicer.getInfo",
  "token": "…64 hex characters…",
  "params": {}
}
```

| Field | Required | Notes |
|---|---|---|
| `v` | no | Protocol version. Reserved; only `1` exists. |
| `id` | when you want a reply | Opaque; echoed back verbatim. Omit for fire-and-forget. |
| `method` | **yes** | Must begin with `slicer.`. Anything else is ignored, not rejected — the channel is shared. |
| `token` | **yes** | `window.__slicerBridge.token`. See §6. |
| `params` | no | Object. Defaults to `{}`. |

For compatibility with the older envelope used elsewhere in this codebase, `command` is accepted as
an alias for `method` and `data` for `params`. New code should not use them.

### Response (slicer → page)

```json
{
  "type": "response",
  "v": 1,
  "id": "wa-7",
  "method": "slicer.getInfo",
  "code": 0,
  "message": "success",
  "data": { }
}
```

A response is sent only when the request carried an `id`. `code` is `0` on success.

### Event (slicer → page)

```json
{ "type": "event", "v": 1, "id": "wa-7", "method": "slicer.projectOpened", "data": { "ok": true } }
```

Events carry the originating `id` when they report on a specific request. Subscribe with
`window.__slicerBridge.on("slicer.projectOpened", fn)`.

---

## 4. Methods

### `slicer.getInfo`

No parameters. Never fails on a trusted origin.

```json
{
  "api":      { "version": 1, "envelope": "slicer-rpc/1" },
  "app":      { "name": "Unlayered Slicer", "version": "0.0.3-dev", "coreVersion": "02.06.00.51",
                "sourceUrl": "https://github.com/Unlayered3D/Unlayered-Slicer", "license": "AGPL-3.0" },
  "platform": { "os": "windows", "locale": "en", "darkMode": true },
  "capabilities": ["slicer.getInfo", "slicer.openProjectFromUrl", "slicer.selectPreset", "slicer.openProject"],
  "limits":   { "maxDownloadBytes": 268435456,
                "fileTypes": ["3mf","stl","step","stp","obj","amf","svg","drc","gcode"] },
  "origin":   { "configured": "http://127.0.0.1:8765", "trusted": true, "loopback": true },
  "tab":      { "slot": "web_app", "title": "Unlayered Tools" },
  "settingsAccess": false
}
```

**`capabilities` is computed at run time and is the only correct way to decide what you may call.**
It varies with the configured origin and the user's settings. Do not hard-code a method list.
With the settings opt-in on (§4.1) it grows by the eight settings methods; `slicer.capabilitiesChanged`
(§4.5) tells you when to re-read it.

`platform.os` is one of `windows`, `macos`, `linux`. `platform.locale` is the slicer's active
language code. `app.sourceUrl` is described in §1. `tab.slot` is `agent_app` (the *Agent* tab),
`universe_app` (the *Universe* tab) or `web_app` (the generic *Web application* slot), so one page
can be served to any of them; `tab.title` is
what the tab is labelled. `settingsAccess` mirrors whether the settings permission is on for this
tab.

### `slicer.openProjectFromUrl`

**The recommended way to hand a file to the slicer**, including when your server runs on the same
machine. It needs no filesystem configuration, and the same code path works once your application
moves to a server.

```json
{ "url": "http://127.0.0.1:8765/download/9f2c…", "filename": "bracket.3mf",
  "sha256": "e3b0c44298fc1c149afbf4c8996fb924…" }
```

| Parameter | Required | Notes |
|---|---|---|
| `url` | **yes** | `http` or `https`. The host must match the page's own host, unless it is listed in `web_app_download_hosts`. |
| `filename` | recommended | Reduced to a bare filename; path separators are stripped. Falls back to a name derived from the URL. The extension must be in `limits.fileTypes`. |
| `sha256` | optional but advised | Lowercase hex. On mismatch the file is deleted and never opened. |

The file is downloaded to `<data dir>/cache/webapp/`, then opened: `.3mf` as a project, `.gcode` in
the **Preview tab in G-code-preview mode** (the sidebar is greyed and Slice/Print disabled until
File ▸ New Project — the same state as opening a G-code file from disk), everything else onto the
current plate.

Two things to know about the G-code case. Opening G-code first closes the current project, so a
project with unsaved changes shows the slicer's "save changes?" dialog (and modified presets a second
one) before anything appears; a user who cancels there still gets `ok: true` — see below. And the
slicer **ignores a G-code file whose name matches the one already shown**, so send a **unique
`filename` per result** (for example `part-3f6a2c1d.gcode`), including when the same model is sliced
again — otherwise the second result is silently dropped.

A handed-over G-code is not a dead end. In the preview the **Print** button uploads it to the
printer configured in Printer Settings, and **Export G-code file** saves it wherever the user likes
— the same two actions a locally sliced plate offers, operating on the file you sent. Slicing stays
disabled, since there is no model to slice.

Emits `slicer.openProjectFromUrl.progress` roughly once per received mebibyte:

```json
{ "received": 3145728, "total": 8388608 }
```

`total` is `0` when the server sends no `Content-Length`.

### `slicer.openProject`

Opens a file already on the user's disk. Present in `capabilities` **only** when the page origin is
loopback, or when the user has enabled *Allow the web application to open local files*.

```json
{ "path": "C:\\Users\\me\\AppData\\Roaming\\Unlayered Slicer\\cache\\webapp\\bracket.3mf" }
```

The path is canonicalised and then checked: it must be a regular file, of an accepted type, within
`limits.maxDownloadBytes`, not a UNC path, and underneath one of the roots in `web_app_path_roots`.
When that setting is empty the roots default to the slicer's `cache/webapp` folder, the system
temporary directory, and the user's configured download folder.

**Files elsewhere are rejected with `1003`, by design.** If your application writes files to its own
working directory, either serve them over HTTP and use `slicer.openProjectFromUrl`, or ask the user
to add that directory to `web_app_path_roots`. The first is almost always the better answer.

### `slicer.selectPreset`

Switches the application to an **already-installed** preset, by name or by the alias the dropdown
shows. There is no way to create, modify, delete or import one.

```json
{ "type": "printer",  "name": "Unlayered 5 Axis Printer 0.4 nozzle" }
{ "type": "print",    "name": "0.20mm Standard @Unlayered", "ifDirty": "ask" }
{ "type": "filament", "name": "Generic PETG", "slot": 1, "ifDirty": "discard" }
```

| Parameter | Required | Notes |
|---|---|---|
| `type` | no | `"printer"` (the default), `"print"` or `"filament"`. The printer form is protocol v1 and always available; the print and filament forms belong to the settings family (§4.1) and need its opt-in, else `1005`. |
| `name` | **yes** | An installed preset's name or alias; a `(modified)` suffix is ignored. Else `1008`. |
| `slot` | filament only | Sidebar slot index, default `0`. Out of range → `1002`. |
| `ifDirty` | no | What to do when the preset being replaced has unsaved edits: `"ask"` (default) answers `1013` and changes nothing; `"discard"` drops the edits and proceeds; `"prompt"` shows the slicer's own "transfer or discard?" dialog. While the tab has **no** settings opt-in this is always `"prompt"`, so a v1 client sees v1 behaviour. |

```json
{ "type": "printer", "selected": "Unlayered 5 Axis Printer 0.4 nozzle", "changed": true }
{ "type": "filament", "slot": 1, "selected": "Generic PETG", "changed": true }
```

`selected` is the preset actually in force **after** the call, which is not always the one you
asked for: under `"prompt"` the user may decline the dialog, and a filament switch on a
single-filament printer goes through the filament tab. Read `selected` rather than assuming.
Selecting the preset that is already selected changes nothing and reports `changed: false`. On a
multi-filament printer a filament switch changes exactly the slot you named.

Why you would want the printer form: the bed drawn in the preview, and the coordinates the
toolpaths are judged against, come from the selected printer preset. Opening G-code does not change
that selection, so a page that slices for one printer while the application sits on another
produces a preview on the wrong bed. Call this before `slicer.openProjectFromUrl`.

### 4.1 Preset settings — behind a per-tab permission

The eight methods below (and the print/filament forms of `selectPreset`) appear in `capabilities`
only for a tab whose `*_allow_settings` key is on. It ships on for the Agent tab, whose whole
purpose is to change settings on the user's behalf, and off for Universe and for the generic slot.
Without it every one of them answers `1005`, and `slicer.getInfo` reports `settingsAccess: false`.

Rules shared by all of them:

- `type` is `"printer"`, `"print"` or `"filament"` — the three sidebar presets.
- Values cross the bridge as **strings in the preset's own `.ini` form**: `"0.2"`, `"35%"`, `"1"`
  for a bool, `"1,0"` for a per-extruder bool vector, `"0x0,220x0,220x220,0x220"` for points.
  `setSettings` also accepts numbers, booleans and arrays and serialises them for you.
- Three deny lists apply everywhere and no setting widens them
  ([`src/slic3r/GUI/WebAppSettings.cpp`](src/slic3r/GUI/WebAppSettings.cpp)):
  - **secret** — `print_host`, `print_host_webui`, `printhost_apikey`, `printhost_cafile`,
    `printhost_user`, `printhost_password`, `printhost_port`, `printhost_ssl_ignore_revoke`,
    `printhost_authorization_type`, `bbl_use_printhost`, `host_type`: never read, never written;
  - **code** — the sixteen custom-G-code options (`machine_start_gcode`, `machine_end_gcode`,
    `layer_change_gcode`, `filament_start_gcode`, … `filament_change_extrusion_role_gcode`) and
    `post_process`: never written, read only when the call says `includeCode: true`;
  - **identity** — `inherits`, `printer_model`, `printer_variant`, `printer_technology`,
    `compatible_prints`, `compatible_printers`, `compatible_prints_condition`,
    `compatible_printers_condition`, `filament_vendor`, `filament_id` and every `*_settings_id`:
    readable, never written. Read-only and plugin-backed options are never written either.
- A key the slicer does not know is `1008`; a denied one is `1003`.
- Every call answers `1007` while a modal dialog is open (`"a dialog is open"`); the writers
  (`setSettings`, `selectPreset`, `slice`) also while the slicer is slicing.
- Writes are **unsaved edits**, exactly what typing the value into the sidebar would produce: the
  tab shows "(modified)", the user can undo or save, and the bridge writes nothing to disk.
- Labels, tooltips and categories in the schema are the slicer's untranslated source strings.

#### `slicer.listPresets`

```json
{ "type": "filament", "query": "PETG", "compatibleOnly": true, "limit": 50 }
```

`query` is a case-insensitive substring of the name or alias; `compatibleOnly` (default `true`)
drops presets incompatible with the selected printer; `limit` defaults to 500. Only visible
presets are listed.

```json
{ "type": "filament", "selected": "Generic PETG", "edited": { "name": "Generic PETG", "dirty": false },
  "total": 3,
  "presets": [ { "name": "Generic PETG", "alias": "Generic PETG", "isSystem": true, "isUser": false,
                 "isCompatible": true, "isVisible": true } ],
  "slots": ["Generic PLA", "Generic PETG"], "slotColours": ["#FFFFFF", "#00AE42"] }
```

`slots` and `slotColours` are present for `filament` only: the preset in each sidebar slot and
its project colour. `total` counts matches before `limit`.

#### `slicer.getSettingsSchema`

```json
{ "type": "print", "query": "infill", "includeCode": false, "limit": 200 }
{ "type": "print", "keys": ["layer_height", "sparse_infill_density"] }
{ "type": "printer", "category": "Extruder" }
```

Give `keys`, or filter the tab's own keys by `category` (exact, case-insensitive) and/or `query`
(substring of key, label or tooltip). `limit` defaults to 200 (`truncated: true` when it bit).
Each option comes from the slicer's option definitions:

```json
{ "type": "print", "total": 14, "truncated": false,
  "options": [ { "key": "sparse_infill_density", "type": "percent", "vector": false, "nullable": false,
                 "label": "Sparse infill density", "fullLabel": "", "category": "Strength",
                 "tooltip": "Density of internal sparse infill, 100% turns all sparse infill into solid infill and internal solid infill are 100% density",
                 "sidetext": "%", "min": 0, "max": 100, "mode": "simple",
                 "enumValues": [], "enumLabels": [], "readonly": false, "writable": true, "value": "15%" } ] }
```

`type` is one of `float`, `floats`, `int`, `ints`, `string`, `strings`, `percent`, `percents`,
`float_or_percent`, `floats_or_percents`, `point`, `points`, `point3`, `bool`, `bools`, `enum`,
`enums`, `points_groups`, `ints_groups`. `min`/`max` are `null` when unbounded. `writable` is the
deny-list verdict; `value` is the current serialised value, `null` when this tab's preset does not
hold the key.

#### `slicer.getSettings`

```json
{ "type": "print", "keys": ["layer_height", "wall_loops"] }
{ "type": "filament", "slot": 1 }
```

Without `keys`, every readable key of the preset. `slot` (filament) reads the preset in that
sidebar slot; when it is not the one the Filament tab is editing you get its *saved* values, since
only the edited preset carries unsaved edits (`preset.source` names which preset you got).

```json
{ "type": "print",
  "preset": { "selected": "0.20mm Standard @Unlayered", "edited": "0.20mm Standard @Unlayered",
              "dirty": true, "dirtyKeys": ["wall_loops"], "source": "0.20mm Standard @Unlayered" },
  "values": { "layer_height": "0.2", "wall_loops": "4" } }
```

#### `slicer.setSettings`

```json
{ "type": "print", "values": { "sparse_infill_density": "35%", "wall_loops": 4, "enable_support": true } }
{ "type": "filament", "slot": 1, "values": { "nozzle_temperature": [235] }, "ifDirty": "ask" }
```

`values` holds 1 to 64 entries. Each is parsed by the slicer's own deserialiser (so it is exactly
as strict as a `.ini` file) and range-checked against the option's `min`/`max`; a value that fails
is reported in `rejected` and **nothing** of that key is applied. Keys are independent: one bad
key does not stop the others. `slot` (filament) first puts the Filament tab on that slot's preset,
which is a preset switch and therefore under `ifDirty` (`1013` by default when the tab has unsaved
edits; `1006` if the user declines under `"prompt"`).

```json
{ "applied": ["sparse_infill_density", "wall_loops"],
  "rejected": [ { "key": "print_host", "code": 1003, "message": "print-host address, credential or upload switch: never exposed over the bridge" },
                { "key": "layer_height", "code": 1002, "message": "value out of range [0.001, inf]" } ],
  "preset": { "type": "print", "selected": "0.20mm Standard @Unlayered", "dirty": true } }
```

When no key at all was applied the call fails with `1002` and the same `rejected` list in `data`.
A successful write is followed by `slicer.settingsChanged` with `source: "bridge"` (§4.5).

#### `slicer.getPlate`

No parameters.

```json
{ "printerTechnology": "FFF", "projectName": "bracket", "projectDirty": true, "isEmpty": false,
  "currentPlate": 0, "plateCount": 2, "bedMm": { "width": 220, "depth": 220, "height": 250 },
  "plates": [ { "index": 0, "isCurrent": true, "name": "", "hasPrintableInstances": true, "sliceResultValid": false } ],
  "objects": [ { "index": 0, "name": "bracket.stl", "plate": 0, "printable": true, "instances": 1,
                 "bboxMm": { "min": [90.1, 90.1, 0], "max": [129.9, 129.9, 12], "size": [39.8, 39.8, 12] } } ],
  "objectsTruncated": false }
```

`objects` is capped at 100 (`objectsTruncated`). `plate: -1` means the object sits on no plate.

#### `slicer.slice`

```json
{ "plate": 0 }
```

`plate` (optional) selects that plate first. Refused with `1008` when the plate has nothing
printable or a G-code file is open in preview mode, `1007` while a slice is running. Then it does
what the Slice button does — which may decline for reasons the slicer shows as a notification —
and answers immediately:

```json
{ "accepted": true, "plate": 0, "sliceResultValid": false }
```

`accepted: false` means no slice is under way (the plate was already sliced and unchanged, or the
slicer refused). The outcome arrives as `slicer.sliceStarted` then `slicer.sliceFinished` (§4.5);
wait for those, not for the response.

#### `slicer.getSliceResult`

```json
{ "plate": 0 }
```

`1008` unless that plate (default: the current one) holds a valid slice result.

```json
{ "plate": 0, "timeSeconds": 2711.4, "prepareTimeSeconds": 61.2, "filamentMm": 4123.7, "filamentG": 12.3,
  "cost": 0.31, "filamentChanges": 0, "travelDistanceMm": 9876.5, "toolpathOutside": false, "gcodeCheckError": 0 }
```

### Completion is two-stage

`code: 0` from either open method means **accepted, not finished**. The slicer may put a modal in
front of the user ("discard the current project?") or spend a while downloading. The outcome
arrives as an event carrying the same `id`:

```json
{ "type": "event", "id": "wa-7", "method": "slicer.projectOpened",
  "data": { "ok": true, "path": "…" } }

{ "type": "event", "id": "wa-7", "method": "slicer.projectOpened",
  "data": { "ok": false, "error": "checksum mismatch: expected …, got …" } }
```

Keep your acknowledgement timeout short and wait on the event for the real result.

**What `ok: true` does and does not promise.** It means the file was fetched,
checksum-verified if you supplied one, and handed to the slicer's own open path. That
path then runs asynchronously and may put a "discard the current project?" modal in
front of the user. The slicer offers no completion callback there, so **a user who
cancels that modal still produces `ok: true`.** Everything that can go wrong before
that point — forbidden host, unsupported type, missing file, failed download,
checksum mismatch, busy slicer — is reported accurately as `ok: false` with a reason.

Design your UI around that: "handed to the slicer" is a safe thing to say on
`ok: true`; "the model is now on the plate" is not. This is a known limitation of v1
rather than a guarantee to be tightened later without notice.

---

### 4.2 Publishing the open project — behind a separate per-tab opt-in

Three methods let a page take what is on the plate, so it can be shared without the user saving a
3MF by hand and finding it again in a file picker. They appear in `capabilities` only when the
tab's `<slot>_allow_export` is on, and answer `1005` while it is off.

This is a **separate permission from `allow_settings`**, because it is a different thing to agree
to: the user's model leaving the machine, rather than the slicer's own settings being read. A tab
may hold either without the other.

The page never names a path. The slicer chooses where the export goes, writes it into its own
cache directory and returns an opaque `handle`; every later call is in terms of that handle. This
is why the family is not the `slicer.readFile` the bridge refuses to grow — it can only hand back
a file the slicer just produced from state the user already has open.

#### `slicer.exportProject`

Params: `{plate?}` — one plate by index, or the whole project when omitted.

Returns `{handle, filename, sizeBytes, chunkBytes}`. `filename` is derived from the project name.
Errors: `1005` not permitted, `1008` the plate is empty, `1007` a slice is running or too many
exports are still open (three at once per tab), `1002` the export is over the size limit, `1012`
the slicer could not export.

The export runs with the slicer's silent save strategy, so it never raises a dialog of its own.

#### `slicer.readExportChunk`

Params: `{handle, offset?, length?}`. Returns `{handle, offset, length, nextOffset, eof, base64}`.

Reads are capped at `chunkBytes` (4 MB) per call regardless of `length`. The payload is plain
base64, not a data URL. Chunked rather than one response because the bytes cross the process
boundary inside a JSON message: a 100 MB project in one reply would be a 133 MB string and a
visible stall. Loop until `eof`, using `nextOffset` — a chunk that does not advance is a fault,
not a reason to retry.

#### `slicer.exportThumbnail`

Params: `{plate?}` — defaults to the current plate. Returns `{plate, mime, width, height,
sizeBytes, base64}`: the picture the slicer already renders of that plate, as a PNG.

It exists because a sharing site will not take an upload without a photo, and asking someone to
go and find a screenshot of a model the slicer is already drawing is absurd. Small enough to come
back in one response, unlike the project itself.

Errors: `1008` the plate is empty, **or no picture has been rendered yet**. Rendering needs the 3D
canvas, which is not drawn while a web-application tab is in front, so on a cold start there may
be nothing to hand over until the Prepare tab has been shown once. Treat a missing picture as
something the user can supply instead, never as a reason to abandon the upload.

#### `slicer.discardExport`

Params: `{handle}`. Deletes the export. Idempotent, so a page cleaning up after a failure does not
have to work out whether the export survived. The slicer also discards every live export when the
tab closes, so a page that never calls this leaks nothing permanent.

---

## 4.5 Events

Besides the request-correlated events above, the slicer pushes these unsolicited ones (no `id`).
Subscribe with `window.__slicerBridge.on(name, fn)`. The settings-family events — every row below
except the last two — reach a tab only when its settings opt-in is on (§4.1).

| Event | Data | When |
|---|---|---|
| `slicer.presetChanged` | `{ type, selected, dirty, slot? }` | a sidebar preset was switched, by the user or by the bridge. `slot` is present for a multi-filament slot change. |
| `slicer.settingsChanged` | `{ type, keys: [...], source: "user" \| "bridge" }` | a value changed in a settings tab. **`source: "bridge"` is your own write coming back — never read it as user intent.** |
| `slicer.plateChanged` | `{ currentPlate, plateCount, objectCount }` | objects or plates changed. Coalesced: one event per burst of changes. |
| `slicer.sliceStarted` | `{ plate }` | a slice began. |
| `slicer.sliceFinished` | `{ plate, ok, cancelled, error?, stats? }` | a slice ended. `stats` is the `getSliceResult` body when the plate ended with a valid result. |
| `slicer.themeChanged` | `{ darkMode }` | the user toggled light/dark. Sent only to the Unlayered Agent tab, which keeps its page across the toggle instead of reloading. |
| `slicer.capabilitiesChanged` | `{ capabilities }` | a Preferences toggle changed what you may call. Re-read it. |

---

## 5. Error codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1000` | Malformed request — unparseable JSON, or no method |
| `1001` | Unknown method |
| `1002` | Bad parameters |
| `1003` | Forbidden — page origin, download host, or file path not permitted |
| `1004` | Missing or stale session token |
| `1005` | Capability disabled for this origin or configuration |
| `1006` | Cancelled by the user |
| `1007` | Busy — the slicer is slicing, or another download is in flight |
| `1008` | Not found |
| `1009` | Unsupported file type |
| `1010` | I/O or network failure |
| `1011` | Too large |
| `1012` | Internal error |
| `1013` | Needs confirmation — the call would discard unsaved preset edits. `data` is `{ dirty: true, type, dirtyKeys }`; retry with `ifDirty: "discard"` or `"prompt"` (§4.1) |

Plus two produced by the client shim rather than the slicer: `'no-bridge'` and `'timeout'`.

---

## 6. Security model

Five independent layers. Understand them before reporting that something "doesn't work" — most of
the time it is one of these, working.

**Origin allow-list.** Bridge messages are honoured only when the loaded page's origin exactly
equals the configured one, compared as normalised `scheme://host[:port]` strings. Not a prefix
match, not a substring match. On any other origin the shim is not injected at all and every request
returns `1003`.

**Per-document session token.** A fresh 256-bit token is minted on each successful load of a
trusted origin and handed to the page in `window.__slicerBridge.token`. Every request must carry
it. It is invalidated by any navigation or reload, so it cannot be replayed, and a cross-origin
frame that can reach `window.wx` cannot read the main frame's JavaScript to steal it. Requests
without it return `1004`.

**Transport.** `https` is accepted anywhere. Plain `http` is accepted **only** for loopback hosts
(`127.0.0.0/8`, `::1`, `localhost`), which browsers treat as trustworthy origins. An `http://`
address on the network is refused before any connection is attempted, and there is deliberately no
override. Certificate errors are fatal — there is no click-through.

**Navigation containment.** The tab may navigate only within the configured origin, plus any
https origin listed in `web_app_extra_origins`. Everything else is vetoed and handed to the system
browser, as are all pop-ups. This is what stops the trusted origin drifting out from under the
checks above.

**Navigable is not trusted.** `web_app_extra_origins` exists so an application can send the user to
a vendor page, a documentation site or an identity provider without ejecting them from the window.
It widens *navigation only*. The bridge shim is injected solely when the origin allow-list above
matches exactly, so a page reached this way has no `window.__slicerBridge`, no session token, and
every `slicer.*` call it attempts returns `1003`. Keep the two lists straight when reading the
code: `is_navigable()` consults both, `origin_is_allowed()` never does.

Because the tab has no address bar, a bar appears across the top whenever the user is on one of
these origins. It names the site, offers a way back to the application, and offers to reopen the
current page in the user's real browser. Do not remove it: without it the user cannot tell whose
site they are on — which matters most on a page asking for a password or a card number — and has
no way back.

**Capability gating.** `slicer.openProject` requires a loopback origin or an explicit user opt-in,
and is absent from `capabilities` when unavailable. Downloads are same-host by default. The
preset-settings family (§4.1) requires its own per-tab opt-in, is absent from `capabilities`
without it, and is filtered by deny lists that no configuration widens. Each tab is governed by
its own settings (`web_app_*` or `agent_app_*`): a page in one tab cannot see or use the other's.

### Deliberately absent

There is no way to read or write arbitrary files, run a process, open an arbitrary path in the
shell, read or modify the slicer's own configuration (AppConfig — print hosts, download folders,
the tab URLs themselves), reach anything to do with accounts, cloud tokens or the network agent,
write custom G-code or `post_process` scripts into a preset, or read a print host's address or
credentials. This is not an oversight and these are not on a roadmap. The list, and the reasoning
for each item — including why preset *settings* are exposed and exactly how narrowly — is kept in
the header comment of [`src/slic3r/GUI/WebAppBridge.hpp`](src/slic3r/GUI/WebAppBridge.hpp).

---

## 7. Configuration

In the slicer's `.conf`, edited while the application is closed. There is no Preferences UI for
these (see below).

| Key | Default | Meaning |
|---|---|---|
| `web_app_enabled` | seeded | Show the tab. Requires a restart. |
| `web_app_url` | seeded | The address to load. |
| `web_app_title` | seeded | The tab label. |
| `web_app_allow_local_files` | `false` | Permit `slicer.openProject` from a non-loopback origin. |
| `web_app_path_roots` | empty | `;`-separated roots that `slicer.openProject` may read from. Empty means the conservative default in §4. |
| `web_app_download_hosts` | empty | `;`-separated extra hosts `slicer.openProjectFromUrl` may fetch from. |
| `web_app_extra_origins` | empty | `;`-separated extra **https** origins the tab may navigate to. See below. |
| `web_app_allow_settings` | `false` | Open the preset-settings family (§4.1) to this tab. |
| `web_app_seeded` | — | Internal. Set once the shipped defaults have been applied. |

The tab appears when `web_app_enabled` is set **and** `web_app_url` is non-empty.

The **Agent** tab is a second instance of the same panel and bridge over its own keys:
`agent_app_enabled`, `agent_app_url`, `agent_app_title`, `agent_app_allow_local_files`,
`agent_app_path_roots`, `agent_app_download_hosts`, `agent_app_extra_origins`,
`agent_app_allow_settings`, `agent_app_seeded` — same meanings. No two tabs share a setting;
`slicer.getInfo` reports which one a page is in as `tab.slot`.

The **Universe** tab is a third instance, over `universe_app_*` with the same key names and
meanings. It ships with `allow_settings` off: it browses and imports models and has no reason to
read or change print settings.

**Where the shipped values come from.** Not from the seed files, and not from Preferences. Each
slot carries its own `default_url`, `default_title` and permissions as fields on `WebAppSlot`, and
`WebAppPanel::ensure_seeded` writes them into the configuration. The seed files under
`resources/web/webapp/` are a development overlay read on top where they exist; they are excluded
from the installer, so on a customer machine the slot is the only source there is. There is no
Preferences UI for any of this: Agent and Universe each have one sensible address, and the generic
slot is a developer harness.

The address and the label are **authoritative** — rewritten whenever they differ from what the slot
says, not merely filled in when the key is missing. Without a Preferences UI a stored value cannot
be a choice the user made; it can only be what an earlier build seeded, so deferring to it would
mean a renamed tab or a moved address reached fresh installations and no others. The permissions
are not treated this way: `*_allow_settings` is applied on a first seed only, and
`*_allow_export` whenever the key has no value at all.

Whether a tab appears is the `*_enabled` key, which is seeded on a **first seed only** — a bool has
no unset state, so a stored `false` cannot be told from the user having switched it off. That is
useless for adding or withdrawing a tab across releases, so a slot may also carry a
`visibility_once` tag with a direction. When the tag differs from what `*_disable_once` records,
`enabled` is set to the slot's direction exactly once and the tag is stored; switching a tab off
clears its address too, so it cannot be brought back by flipping one boolean in a config file.

`universe_app_allow_export` is the publish permission: it opens `slicer.exportProject`,
`slicer.readExportChunk` and `slicer.discardExport` to the tab, which is how a page offers "use
what is on my plate" instead of asking the user to save a 3MF and find it again. It ships on for
Universe and off for the other two slots. Unlike `enabled` and `allow_settings` it is seeded
whenever the configuration has **no value** for it rather than only on a first seed, because it is
newer than the installations it has to reach; a value the user has actually chosen is left alone.

`universe_app_download_hosts` is the other key that tab genuinely needs. Its site answers a download
with a redirect to a short-lived signed URL on a **separate storage host**, and
`slicer.openProjectFromUrl` allows only the page's own host plus whatever is named here — so
without it, every "open in the slicer" is refused with `1003`. A local copy of the site serving
its own files needs nothing, because that is the page's own host.

Development overrides, read from the environment and never written back to the configuration:
`ORCA_WEB_APP_URL`, `ORCA_WEB_APP_TITLE`, `ORCA_AGENT_APP_URL`, `ORCA_AGENT_APP_TITLE`,
`ORCA_UNIVERSE_APP_URL`, `ORCA_UNIVERSE_APP_TITLE`, `ORCA_WEB_APP_DEVTOOLS=1`. Setting a `*_URL`
override enables that tab regardless of its `*_enabled` key.

Developer tools are available in the tab when Preferences ▸ Debug ▸ developer mode is on, or
`ORCA_WEB_APP_DEVTOOLS=1` is set: right-click ▸ Inspect.

---

## 8. Worked example

```js
async function handOff(downloadUrl, name, sha256) {
  const bridge = window.__slicerBridge;
  if (!bridge) return false;                       // ordinary browser: leave the link alone

  const info = await bridge.send("slicer.getInfo");
  if (!info.capabilities.includes("slicer.openProjectFromUrl")) return false;

  const done = new Promise(resolve => {
    const onOpened = data => { bridge.off("slicer.projectOpened", onOpened); resolve(data); };
    bridge.on("slicer.projectOpened", onOpened);
  });

  bridge.on("slicer.openProjectFromUrl.progress",
            p => setStatus(p.total ? `${Math.round(100 * p.received / p.total)}%` : "downloading…"));

  try {
    await bridge.send("slicer.openProjectFromUrl", { url: downloadUrl, filename: name, sha256 });
  } catch (e) {
    setStatus(`Could not hand over: ${e.message}`);  // the download link stays usable
    return false;
  }

  const result = await done;
  setStatus(result.ok ? "Opened in the slicer" : `Not opened: ${result.error}`);
  return result.ok;
}
```

Two habits worth copying from it: render your own download link **first** and treat the hand-off as
an enhancement on top, and never let a bridge failure block the user from their file.

---

## 9. Versioning

`api.version` is `1`. Within version 1, methods and fields may be **added**; nothing already
specified will change meaning or be removed. Check `capabilities` rather than `app.version`.

An incompatible change would ship as `api.version: 2`, with version 1 still answered for a
transition period.

---

## 10. Implementation

| File | Role |
|---|---|
| [`src/slic3r/GUI/WebAppSlot.hpp`](src/slic3r/GUI/WebAppSlot.hpp) | Every slot: which keys, env overrides and seed file each tab is configured from |
| [`src/slic3r/GUI/WebAppPanel.cpp`](src/slic3r/GUI/WebAppPanel.cpp) | The tab: configuration, navigation, origin tracking, fallback page, event fan-out (`notify_all`) |
| [`src/slic3r/GUI/WebAppBridge.cpp`](src/slic3r/GUI/WebAppBridge.cpp) | The protocol: envelope, token, dispatch, every method, `notify` |
| [`src/slic3r/GUI/WebAppSettings.cpp`](src/slic3r/GUI/WebAppSettings.cpp) | The settings policy: deny lists, readable/writable predicates, schema and slice-summary JSON |
| [`docs/HLSD/web-app-bridge.md`](docs/HLSD/web-app-bridge.md) | High-level design: slots, gate, deny lists, write path, event hooks |
| [`resources/web/webapp/unavailable.html`](resources/web/webapp/unavailable.html) | Shown when the application cannot be reached |
| [`src/slic3r/GUI/WebAppSlot.hpp`](src/slic3r/GUI/WebAppSlot.hpp) | The slots: keys, environment overrides and the defaults each tab ships with |
| [`resources/web/webapp/*.json`](resources/web/webapp/) | Development overlay on the slot defaults; not shipped in the installer |
| [`resources/web/webapp-example/`](resources/web/webapp-example/) | MIT-licensed reference client; exercises every method and event |
