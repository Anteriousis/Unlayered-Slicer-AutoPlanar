# Web App Bridge — High Level Design

## Why it exists

Unlayered Slicer hosts web applications as top-level tabs — a browser control pointed at a URL
the user configures, the same way the Device tab embeds a printer's own web interface. A hosted
page needs a few things only the slicer can do for it: open the file it produced, put the
application on the right printer, and — for the Unlayered Agent, a chat that edits print settings
— read and change the values in the Print / Filament / Printer tabs and start a slice.

The bridge is the whole of what such a page can ask for: a small JSON-RPC surface over the web
view's `window.wx.postMessage` channel, specified in `WEB_APP_BRIDGE.md` at the repository root
and exercised by the MIT-licensed reference client in `resources/web/webapp-example/`. Everything
about it is shaped by one fact: the page is a **separate program**, possibly on a remote server,
possibly not written by us. The slicer is AGPL-3.0; the page is whatever its author likes. The
interface therefore exposes capabilities, never internals, and every widening of it is argued in
the header comment of `src/slic3r/GUI/WebAppBridge.hpp`.

## The slots

There is one panel class, `WebAppPanel`, and one bridge class, `WebAppBridge`, instantiated once
per **slot**. A slot (`WebAppSlot`, a struct of `const char*`) is the row of AppConfig keys,
environment overrides and seed file a panel is configured from. Three ship:

| Slot | Tab | Keys | Ships | Theme toggle |
|---|---|---|---|---|
| `agent()` (`agent_app`) | "Agent" | `agent_app_*` | on, with the settings permission | re-themes in place through `slicer.themeChanged`, because the page holds a conversation |
| `universe()` (`universe_app`) | "Universe" | `universe_app_*` | on, with the export permission | reloads; browsing state is cheap to rebuild and the site themes itself on load |
| `generic()` (`web_app`) | "Web application", user-configurable, application-agnostic | `web_app_*` | off, with no address | reloads the page, like every other web view |

A slot's shipped values are its own `default_url`, `default_title` and permission fields, written
into the configuration once by `ensure_seeded`. They used to live in `resources/web/webapp/*.json`,
which is where the naming rule in WEB_APP_BRIDGE.md wanted them; those files are excluded from the
installer, so a shipped build found nothing to read and the defaults had to move into the slot. The
files are still read as a development overlay where they exist.

`MainFrame::web_app_slot_view` is the one place that maps a slot to its panel and notebook page,
so a fourth slot is an entry there rather than another branch in each caller.

The Universe tab embeds the model-sharing site so that opening a print puts it on the plate
instead of in the Downloads folder. The page asks its own API for a signed URL and passes that to
`slicer.openProjectFromUrl`; the slicer fetches the file itself. Two constraints shape that. The
slicer downloads with its own HTTP client, which carries none of the web view's cookies, so a
cookie-authenticated download route would answer 401 — the URL handed over has to be one that
stands on its own. And `openProjectFromUrl` allows the page's own host automatically but nothing
else, so a site that redirects downloads to a separate storage host must name it in
`universe_app_download_hosts` or every open is refused with `1003`.

The publish direction is the mirror of that. `slicer.exportProject` writes the open project into
the slicer's cache and returns an opaque handle; the page pulls the bytes with
`slicer.readExportChunk` and drops the assembled file into the ordinary upload form. It sits
behind its own `<slot>_allow_export` gate rather than reusing `allow_settings`, because consenting
to a page reading print settings is not the same as consenting to the model leaving the machine.

Nothing in the slicer's source names a particular application: the URL, the title and every gate
are configuration, and the seed files are data. The bridge reads *its own* slot's keys
(`owner.slot().key_allow_local`, `key_path_roots`, `key_download_hosts`, `key_allow_settings`) so
each tab is governed independently; a page in one tab cannot learn or exploit another's
settings. `slicer.getInfo` tells a page which slot it is in (`tab.slot`) so one application can be
served to both.

## Trust, in layers

Requests are honoured only when the loaded document's origin **exactly equals** the configured
one; the origin is checked again before every dispatch. A fresh 256-bit **session token** is minted
on each load of a trusted origin, injected into the page as `window.__slicerBridge.token`, and
required on every request — a navigation or reload invalidates it, and a cross-origin frame that
can reach `window.wx` cannot read it. `https` is accepted anywhere, `http` only on loopback; the
tab may navigate only within the configured origin plus listed extra https origins, and those are
**navigable, not trusted** — no token is injected there.

Above that sit **capabilities**: the method list `slicer.getInfo` returns is computed at call time
from the origin and the user's settings, and it is the only correct way for a page to decide what
it may call. A method that is gated off answers `1005` whether or not the page believed it was
available.

## The gate on print settings

Reading and changing preset values is the one place the bridge reaches into the user's own data,
so it is behind an explicit, per-tab, default-off opt-in — `web_app_allow_settings` for the
generic tab, `agent_app_allow_settings` for the agent tab (Preferences ▸ General). With both off the
slicer behaves byte-for-byte as it did before the settings family existed: the capability list is
the original three (four with local files), the events of §"Events" are never sent, and the
printer form of `slicer.selectPreset` keeps its v1 behaviour including its dialog. Toggling the
preference fires `slicer.capabilitiesChanged` so an open page re-reads what it may do.

Behind the gate are eight methods — `listPresets`, `getSettingsSchema`, `getSettings`,
`setSettings`, `getPlate`, `slice`, `getSliceResult` and the print/filament forms of
`selectPreset`. Each begins with the same **preflight**: the gate; "is a modal dialog open"
(`MainFrame::IsEnabled()` is false under one — a heuristic that keeps a bridge call from nesting a
second modal, not a security check); for writers, "is the slicer slicing"; and "does the tab for
this preset type exist".

## What is never exposed: the deny lists

`src/slic3r/GUI/WebAppSettings.cpp` holds three lists that no configuration widens:

- **Secret** — `print_host`, `print_host_webui`, `printhost_apikey`, `printhost_cafile`,
  `printhost_user`, `printhost_password`, `printhost_port` (the seven
  `PresetBundle::full_config_secure()` erases before a config leaves the process) plus
  `printhost_ssl_ignore_revoke`, `printhost_authorization_type`, `bbl_use_printhost`, `host_type`.
  Never read, never written.
- **Code** — the sixteen custom-G-code options of `PrintConfig.cpp` and `post_process`: text the
  slicer emits into a print or runs as a command. Never written; read only when a call says
  `includeCode: true`.
- **Identity** — `inherits`, `printer_model`, `printer_variant`, `printer_technology`, the four
  `compatible_*` keys, `filament_vendor`, `filament_id`, and every `*_settings_id`: what a preset
  *is* rather than what it does. Readable, never written — changing one would turn the edited
  preset into a different preset, which is what `selectPreset` is for.

Two predicates from the option definition complete the write filter: `readonly` options and
plugin-backed options (`ConfigOptionDef::is_plugin_backed()`) are never written either. An
unknown key is `1008`; a denied key is `1003`, named in `rejected[]` with the reason.

Values cross the bridge as the **`.ini` serialisation of a named key** — `"0.2"`, `"35%"`, `"1"`
— never as a `DynamicPrintConfig` dump. A page asks for keys and gets strings; the schema method
tells it the type, range, enum values and labels to make sense of them. This is both the smaller
interface and the one that keeps the two programs two programs.

## The write path

`setSettings` never touches a preset on disk and never bypasses the tab. It copies the tab's
edited config, deserialises each accepted value into the copy with
`ConfigBase::set_deserialize` under the `Disable` substitution rule (so a value the slicer cannot
parse is an error, not a silent default; `handle_legacy` renames still apply), range-checks
numerics with `ConfigOptionDef::is_value_valid`, restores any key that failed, and hands the copy
to **`Tab::load_config()`** — the entry the cut-plane gizmo already uses. That runs
`update_dirty → reload_config → update`, so the result is indistinguishable from the user typing
the value: the tab shows "(modified)", the sidebar's frequently-changed fields follow, the plater
and background process receive the new config, and the user can undo or save as usual.

`Tab::load_config` does not run the couplings `Tab::on_value_change` maintains for GUI field
handlers, so the bridge calls it afterwards for exactly the keys that have one —
`pellet_flow_coefficient` / `filament_diameter` (each rewrites the other; `boost::any<double>`),
the prime-tower / single-extruder-multi-material / purge trio (`boost::any<bool>`) and
`gcode_flavor` (empty `boost::any`) — with the `boost::any` type each handler casts, because a
wrong type there throws.

Replacing the edited preset (`selectPreset` print/filament, `setSettings` on another slot's
filament) is the one operation that can destroy the user's unsaved work, so it is under the
**`ifDirty` contract**: the default `"ask"` answers `1013` with `{dirty, type, dirtyKeys}` and
changes nothing, `"discard"` drops the edits and proceeds, `"prompt"` leaves the slicer's own
"transfer or discard?" dialog to `Tab::select_preset`. A page cannot see the user's edits before it
asks, so the safe default is to be told.

Filament slots have no public selection API of their own in the sidebar; the sequence the sidebar
combo runs (`Plater::priv::on_select_preset`) is extracted into
`Plater::priv::apply_filament_selection`, which the combo and the public
`Plater::select_filament_preset(idx, name)` share. The bridge calls the latter with no combo, so the
`editing_filament` latch `Sidebar::update_presets` relies on never comes into play, and a
multi-filament printer changes exactly the slot asked for.

## Events

The slicer pushes unsolicited events through `WebAppPanel::notify_all`, which fans out to every
live panel's `WebAppBridge::notify`. `notify` is a no-op until the page holds a session token and
drops the settings-family events unless the tab's gate is on. The hooks are where the slicer
already knows the fact, not where the bridge guesses it:

| Event | Hook |
|---|---|
| `presetChanged` | end of `Tab::on_presets_changed`; `apply_filament_selection` when the filament tab did not run (multi-filament slot change) |
| `settingsChanged {source:"user"}` | `Tab::on_value_change`, suppressed while a bridge write is in flight (`WebAppBridgeWriteScope`) so a `setSettings` is reported once, as `source:"bridge"` |
| `plateChanged` | `Plater::priv::object_list_changed`, coalesced onto the next event-loop turn with a pending flag, because it fires several times per user action |
| `sliceStarted` / `sliceFinished` | `Plater::priv::on_slicing_began` / `on_process_completed` after `m_is_slicing` clears, with the slice summary when the result is valid |
| `themeChanged` | the agent slot's `EVT_WEBVIEW_RECREATED` handler, instead of a reload |
| `capabilitiesChanged` | Preferences, after a bridge-relevant toggle |

An application built on these must not read its own `source:"bridge"` echo as user intent; the
field exists so an agent loop cannot argue with itself.

## Relationship with the Unlayered Agent

The agent is a separate Python program (`stealth-slice`) with a React front end. Its server never
runs inside the slicer and the slicer never starts it: the agent tab is a URL, and
`resources/web/webapp/unavailable.html` is what the user sees when nothing answers. Inside the tab,
the agent's model calls **client-executed tools** — the page maps a `slicer_*` tool request from
its server onto the matching `slicer.*` bridge method, waits for the response (and for
`sliceStarted` / `sliceFinished` when it asked for a slice), and posts the result back. A tool is
offered to the model only when the page saw the method in `capabilities`, so a user who has not
opted in sees a chat that talks about settings but cannot touch them, and a plain browser with no
bridge sees the agent's own 3MF pipeline instead. The bridge does not know any of this; it answers
the same eight methods for any page in any tab.

## Constraints that shape it

- **Gates off means unchanged.** With both `*_allow_settings` false the slicer's observable
  behaviour is that of the release before the settings family existed.
- **No AppConfig, no files, no processes, no accounts, no network agent, no G-code writes, no
  print-host keys** — the standing list in `WebAppBridge.hpp`, and the reason the settings family
  is a fixed set of eight methods rather than a generic get/set.
- **GUI thread.** Every handler runs synchronously inside the script-message event; the only
  worker thread is the download, which touches the bridge state through a weak pointer and hops
  back via `CallAfter`. Events are built on the GUI thread and delivered the same way.
- **Additive within protocol v1.** Methods, fields and events are added; nothing specified changes
  meaning. A page checks `capabilities`, not `app.version`.
