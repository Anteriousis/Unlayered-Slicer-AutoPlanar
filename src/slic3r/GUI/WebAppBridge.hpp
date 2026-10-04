#ifndef slic3r_WebAppBridge_hpp_
#define slic3r_WebAppBridge_hpp_

#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>
#include <wx/string.h>

class wxWebViewEvent;

namespace Slic3r {
namespace GUI {

class WebAppPanel;

// Error codes returned to the page. Stable across releases -- see WEB_APP_BRIDGE.md.
enum WebAppBridgeError
{
    WAB_OK                   = 0,
    WAB_BAD_REQUEST          = 1000, // unparseable JSON, or no method
    WAB_UNKNOWN_METHOD       = 1001,
    WAB_BAD_PARAMS           = 1002,
    WAB_FORBIDDEN_ORIGIN     = 1003, // page origin, or download host, not allow-listed
    WAB_BAD_TOKEN            = 1004, // missing or stale session token
    WAB_CAPABILITY_DISABLED  = 1005, // method exists but is gated off for this origin
    WAB_USER_CANCELLED       = 1006,
    WAB_BUSY                 = 1007, // background slicing in progress
    WAB_NOT_FOUND            = 1008,
    WAB_UNSUPPORTED_TYPE     = 1009,
    WAB_IO                   = 1010,
    WAB_TOO_LARGE            = 1011,
    WAB_INTERNAL             = 1012,
    WAB_NEEDS_CONFIRMATION   = 1013, // the call would discard unsaved preset edits; retry with ifDirty
};

// The slicer's side of the Web App bridge: a small, versioned, general-purpose JSON-RPC surface
// that any embedded web application can drive. The protocol is documented in
// WEB_APP_BRIDGE.md and exercised by resources/web/webapp-example.
//
// ---------------------------------------------------------------------------------------------
// Methods that must NOT be added here, and why. Please argue with this list before extending it.
//
//   slicer.readFile / writeFile / listDir / deleteFile
//       Arbitrary filesystem access for a remote page. The whole point of openProjectFromUrl is
//       that the page never needs it. The export family below is not a loophole in this: the
//       page cannot name a path, only read back a file the slicer itself just wrote in its own
//       cache, identified by a handle the slicer minted.
//   slicer.exec / spawn / openPath
//       Arbitrary process execution, or shell-opening an arbitrary path, on behalf of a page.
//   any AppConfig read or write
//       Writing would let a page set print_host, printhost_apikey, download_path -- or the
//       web_app_url / agent_app_url it is itself loaded from, which would make a pivot to an
//       attacker's origin self-perpetuating. Reading exposes slicer_uuid, print host addresses
//       and cloud tokens. Application settings are the user's; the bridge sees none of them.
//   anything touching login, cloud tokens or the network agent.
//   any passthrough to GUI_App::handle_web_request
//       That dispatcher belongs to the Home tab and includes the login commands. Routing this
//       bridge through it would collapse the isolation between the two web views.
//
// The open PROJECT can be handed to the page, deliberately and narrowly. Publishing what is on
// the plate otherwise means saving a 3MF by hand and finding it again in a file picker inside an
// embedded web view, which is worse for the user and no safer. The shape is:
//   - three methods: exportProject, readExportChunk, discardExport. Nothing else moves bytes out.
//   - behind their own per-tab opt-in (<slot>_allow_export, default off, and a separate key from
//     allow_settings because it is a different thing to consent to -- the user's model leaving,
//     rather than the slicer's settings being read). Absent from `capabilities` while it is off.
//   - the slicer chooses the path and the name; the page gets an opaque handle. Reads are bounded
//     per call and the export is deleted on discardExport, or when the tab goes away.
//   - it exports what is already on the plate. It cannot reach a file the user has not opened.
//
// Preset SETTINGS -- the values in the Print / Filament / Printer tabs -- ARE exposed,
// deliberately and narrowly. The policy lives in WebAppSettings.hpp; the shape is:
//   - eight methods: listPresets, getSettingsSchema, getSettings, setSettings, getPlate, slice,
//     getSliceResult, plus the widened selectPreset. Nothing else touches presets.
//   - all of them behind a per-tab opt-in (web_app_allow_settings / agent_app_allow_settings,
//     both default off): absent from `capabilities` and answered with 1005 while it is off, so a
//     slicer with the opt-ins off behaves exactly as before they existed.
//   - three deny lists no configuration widens: print-host addresses and credentials are neither
//     read nor written; custom G-code and post_process are never written and read only on
//     request; preset identity (inherits, printer_model, *_settings_id, compatibility ...) is
//     never written. Read-only and plugin-backed options are never written either.
//   - values cross the bridge as the .ini serialisation of a NAMED key, never as a
//     DynamicPrintConfig dump: the page asks for keys and gets strings.
//   - writes land through Tab::load_config, the entry the cut-plane gizmo uses, so they are
//     ordinary unsaved edits: the tab shows "(modified)", nothing is saved to disk, and the
//     user's own dirty edits are never discarded without ifDirty:"discard" saying so (1013).
//
// slicer.selectPreset EXISTS on those terms (see handle_select_preset): an already-installed
// preset, by name or alias, in the printer, print or filament slot -- no create/modify/delete/
// import -- and it behaves like the user choosing the entry from the dropdown. The printer form
// is the original v1 method and works without the settings opt-in, because a page producing
// G-code for a particular printer otherwise gets a preview drawn on the wrong bed; the print and
// filament forms belong to the settings family and are gated with it.
// ---------------------------------------------------------------------------------------------
class WebAppBridge
{
public:
    explicit WebAppBridge(WebAppPanel& owner);
    ~WebAppBridge();

    // Called after every successful top-level load. Mints and injects a fresh session token when
    // the loaded origin is the configured one.
    void on_document_loaded(const wxString& url);
    void on_script_message(wxWebViewEvent& evt);
    void on_tab_shown(bool shown);

    // Fire-and-forget event to the page (WEB_APP_BRIDGE.md section 4.5). A no-op until a trusted
    // document holds a session token. The settings-family events -- presetChanged,
    // settingsChanged, plateChanged, sliceStarted, sliceFinished -- are also dropped unless this
    // tab's settings opt-in is on. GUI thread; the envelope itself is delivered via CallAfter.
    void notify(const std::string& method, const nlohmann::json& data);
    // slicer.capabilitiesChanged with the current list, for Preferences to call after a toggle.
    void notify_capabilities();

private:
    // shared_ptr, not unique_ptr: an Http worker thread keeps the state alive for the duration of
    // a download callback. See the lifetime note in WebAppBridge.cpp.
    struct Impl;
    std::shared_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_WebAppBridge_hpp_
