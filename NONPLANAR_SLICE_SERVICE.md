# Nonplanar slicing service — protocol v1

Unlayered Slicer can hand the current plate to an **external slicing service** over HTTP and open
the G-code that comes back. This document is the complete specification of that interface.

It exists because some slicing strategies are not layer-by-layer plane sweeps and are not
implemented in this program: a volumetric or five-axis engine takes the same solid and produces
toolpaths that tilt out of the XY plane. Rather than embed one, the slicer can ask a service for
the answer, exactly as it can ask a print host to accept a file.

**Any service at any address may implement this.** There is no vendor allow-list, no registration
and no key. The address is configuration (`nonplanar_slice_url`); nothing in the slicer's source
names a particular service, and the slicer ships with no address at all — with none configured, the
feature is absent from the UI.

---

## 1. Relationship between the two programs

Unlayered Slicer is licensed under the **GNU Affero General Public License, version 3**. A slicing
service it talks to across this interface is a **separate program**: its own process, usually its
own machine, written and licensed however its author likes, exchanging nothing with the slicer but
a mesh file and JSON over a documented protocol. That is the same relationship the program already
has with the print hosts it uploads to (Duet, OctoPrint, and the rest), and nothing here changes it.

If you host a service that runs **this slicer** on your server to produce output for remote users,
AGPL-3.0 §13 asks you to offer those users its Corresponding Source; this interface does not, since
the slicer runs on the user's own machine and the service does the slicing.

---

## 2. Configuration

| Key | Default | Meaning |
|---|---|---|
| `nonplanar_slice_url` | empty | Service root, e.g. `https://example.com/vise` or `http://localhost:8000`. Empty disables the feature. |

Set it in Preferences ▸ General ▸ Nonplanar slicing, or directly in the slicer's `.conf`.
`ORCA_NONPLANAR_SLICE_URL` overrides it for development and is never written back.

**Transport.** `https` is accepted anywhere. Plain `http` is accepted **only** for loopback hosts
(`127.0.0.0/8`, `::1`, `localhost`) — the same rule the web-app tab applies. An `http://` address on
the network is refused before any connection is attempted, and there is deliberately no override.

A trailing slash on the root is ignored. Every path below is relative to it.

---

## 3. Flow

```
POST <root>/slice          multipart: the plate as a mesh + the preset names   -> 202 {job_id}
GET  <root>/jobs/{id}      poll until status is succeeded or failed
GET  <root>/jobs/{id}/gcode   download the result, then it opens in the preview
```

The slicer polls; there is no callback, and the service needs no inbound access to the machine.

### `POST <root>/slice`

`multipart/form-data`:

| Field | Notes |
|---|---|
| `mesh` | **The plate**, as a binary `.3mf` file. Oriented and positioned as the user placed it. One file. |
| `settings` | **The resolved print configuration**, as a JSON file. Every setting, already merged. |
| `printer` | Name of the selected printer preset — a label. |
| `process` | Name of the selected process preset — a label. |
| `filament` | Name of the first selected filament preset — a label. |

`settings` is the authoritative one, and it carries **values, not preset names**: one flat JSON
object of setting keys, the same document a project carries as
`Metadata/project_settings.config`, written by the same serializer. A service reading those
already needs nothing new.

This is deliberate. Preset names would oblige a service to see the user's preset library, which a
hosted service never can and a local one can only by reading another program's data directory —
and every printer a user configures for themselves would be unresolvable there. Sending the values
removes that whole class of failure: the two programs never have to agree on a profile catalogue,
only on what the setting keys mean.

The three names come along as labels, for a job record or a progress message. A service may use
them for display, but it should not need them to slice, and it must not fail a request that
carries settings just because a name is unfamiliar.

Expected response, `202`:

```json
{ "job_id": "79cbadab8ab8462ab79880e045a86e31", "status": "queued" }
```

Any other status is an error; the body is shown to the user, so make it a sentence rather than a
stack trace. A JSON body of the shape `{"detail": "..."}` has its `detail` shown on its own.

### `GET <root>/jobs/{id}`

```json
{
  "status": "succeeded",
  "result": { "filename": "part-79cbadab.gcode", "size": 8520017, "sha256": "0013…6168" },
  "warnings": ["…"]
}
```

`status` is one of **`queued`**, **`running`**, **`succeeded`**, **`failed`** — those four spellings
exactly. On `failed`, `error` carries the reason and is shown to the user. `result` and `warnings`
are optional; `warnings` are shown but do not stop the result being opened.

### `GET <root>/jobs/{id}/gcode`

The G-code, as `text/plain`. `Content-Length` is used for download progress. `HEAD` on the same
path should answer the same headers without side effects.

The file is saved under `<data dir>/cache/nonplanar/` and opened in the preview, where it can be
printed or exported like any sliced plate. The local name always carries the job id, because the
preview ignores a G-code file whose name matches the one already shown.

---

## 4. What the slicer does and does not do

- It sends the **whole plate**, not one object. A service that flattens it into a single solid
  should say so in its own UI.
- It does **not** slice locally first. Nothing about the local slice — its settings validation, its
  time estimate, its G-code — takes part.
- The result opens in **G-code preview mode**: the model tree and Slice are inert until File ▸ New
  Project, while Print and Export G-code work on the returned file.
- The preview draws the bed of the **selected printer preset**. A service that places the part in a
  frame of its own (a five-axis engine centres on the rotary origin, not a bed corner) will look
  misplaced against a printer whose bed does not share that origin. Send the geometry in the frame
  the chosen printer expects.
- Progress is a percentage guess while polling, because the protocol carries no progress figure. A
  future version may add one; a service may not assume one is read.

---

## 5. Implementation

| File | Role |
|---|---|
| `src/slic3r/Utils/NonplanarSliceClient.{hpp,cpp}` | the protocol: submit, poll, download, and the address/transport rules |
| `src/slic3r/GUI/Jobs/NonplanarSliceJob.{hpp,cpp}` | runs it on a worker thread, reports progress, opens the result |
| `Plater::slice_nonplanar()` | exports the plate and starts the job |
