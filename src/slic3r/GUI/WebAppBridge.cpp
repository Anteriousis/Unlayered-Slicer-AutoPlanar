#include "WebAppBridge.hpp"
#include "WebAppPanel.hpp"
#include "WebAppPresetShare.hpp"
#include "WebAppSettings.hpp"
#include "WebAppSlot.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/GUI.hpp" // from_path
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/ModelUploadConsentDialog.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp" // sha256_hex
#include "libslic3r/GCode/Thumbnails.hpp"     // compress_thumbnail
#include "libslic3r/GCode/ThumbnailData.hpp" // ThumbnailData
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/any.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <vector>
#include <atomic>
#include <cfloat>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <vector>

#include <wx/uri.h>
#include <wx/webview.h>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr int    WAB_PROTOCOL_VERSION   = 1;
constexpr size_t WAB_MAX_DOWNLOAD_BYTES = 256u * 1024u * 1024u;
constexpr size_t WAB_PROGRESS_STEP      = 1024u * 1024u; // emit a progress event per MiB
// A printer profile is a few dozen KiB of JSON; anything near this is not one.
constexpr size_t WAB_MAX_PRESET_BYTES   = 4u * 1024u * 1024u;

// The public home of this program's Corresponding Source. Reported by slicer.getInfo so that an
// embedded application -- including one hosted on a server that runs this slicer for remote
// users -- can render the offer required by AGPL-3.0 sections 6 and 13 without hard-coding it.
const char* const WAB_SOURCE_URL = "https://github.com/Unlayered3D/Unlayered-Slicer";

// "gco"/"g" used to be listed too, but Slic3r::is_gcode_file() (libslic3r/utils.cpp) accepts only
// ".gcode", so Plater::load_gcode() silently refused them: advertising a type that can never open
// is a lie to the page. Keep this list to what open_resolved_file() can actually route.
const char* const WAB_FILE_TYPES[] = {"3mf", "stl", "step", "stp", "obj", "amf", "svg", "drc", "gcode"};

std::string json_string(const json& node, const char* key)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// Defined further down with json_bool; forward-declared here because the preset handlers above that
// point call it.
int json_int(const json& node, const char* key, int fallback);

std::string dump_json(const json& node) { return node.dump(-1, ' ', false, json::error_handler_t::replace); }

fs::path path_from_utf8(const std::string& utf8_path)
{
#ifdef _WIN32
    const wxString wide_path = wxString::FromUTF8(utf8_path.c_str());
    return fs::path(wide_path.ToStdWstring());
#else
    return fs::path(utf8_path);
#endif
}

std::string path_to_utf8(const fs::path& path)
{
#ifdef _WIN32
    return std::string(wxString(path.wstring()).ToUTF8().data());
#else
    return path.string();
#endif
}

std::string make_session_token()
{
    std::random_device rd;
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i)
        oss << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(rd());
    return oss.str(); // 64 hex characters, 256 bits
}

bool extension_allowed(const std::string& lowercase_ext)
{
    for (const char* ext : WAB_FILE_TYPES)
        if (lowercase_ext == ext)
            return true;
    return false;
}

std::string lowercase_extension(const fs::path& path)
{
    std::string ext = path_to_utf8(path.extension());
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return boost::algorithm::to_lower_copy(ext);
}

// Reduce a client-supplied name to a bare filename. Never trusted as a path.
std::string sanitise_filename(const std::string& raw)
{
    std::string name = raw;
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
            c = '_';
    while (!name.empty() && name.front() == '.') // a leading dot is how ".." gets through
        name.erase(name.begin());
    if (name.size() > 128)
        name = name.substr(name.size() - 128);
    return name;
}

fs::path webapp_cache_dir() { return path_from_utf8(data_dir()) / "cache" / "webapp"; }

// Roots that a local path handed to slicer.openProject must sit under. Configurable per tab
// (slot.key_path_roots); when the setting is empty a conservative default applies. There is
// deliberately no "unrestricted" value -- an application wanting to hand over files from
// elsewhere should serve them over HTTP and use slicer.openProjectFromUrl.
std::vector<fs::path> allowed_path_roots(const WebAppSlot& slot)
{
    std::vector<fs::path> roots;
    AppConfig*            cfg = wxGetApp().app_config;

    const std::string configured = cfg != nullptr ? cfg->get(slot.key_path_roots) : std::string();
    if (!configured.empty()) {
        std::vector<std::string> parts;
        boost::algorithm::split(parts, configured, boost::algorithm::is_any_of(";"));
        for (std::string& part : parts) {
            boost::algorithm::trim(part);
            if (part.empty())
                continue;
            boost::system::error_code ec;
            const fs::path            canonical = fs::weakly_canonical(path_from_utf8(part), ec);
            if (!ec)
                roots.push_back(canonical);
        }
        if (!roots.empty())
            return roots;
    }

    boost::system::error_code ec;
    roots.push_back(fs::weakly_canonical(webapp_cache_dir(), ec));
    const fs::path temp = fs::temp_directory_path(ec);
    if (!ec)
        roots.push_back(fs::weakly_canonical(temp, ec));
    if (cfg != nullptr) {
        const std::string downloads = cfg->get("download_path");
        if (!downloads.empty()) {
            const fs::path d = fs::weakly_canonical(path_from_utf8(downloads), ec);
            if (!ec)
                roots.push_back(d);
        }
    }
    return roots;
}

bool path_is_under(const fs::path& candidate, const fs::path& root)
{
    auto c = candidate.begin();
    auto r = root.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end())
            return false;
#ifdef _WIN32
        if (!boost::algorithm::iequals(path_to_utf8(*c), path_to_utf8(*r)))
            return false;
#else
        if (*c != *r)
            return false;
#endif
    }
    return true;
}

std::vector<std::string> configured_download_hosts(const WebAppSlot& slot)
{
    std::vector<std::string> hosts;
    AppConfig*               cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return hosts;
    // Configuration wins; otherwise the slot's own default, so a tab whose storage host the build
    // already knows works without anyone having to type it in.
    std::string configured = cfg->get(slot.key_download_hosts);
    if (configured.empty() && slot.download_hosts != nullptr)
        configured = slot.download_hosts;
    if (configured.empty())
        return hosts;
    boost::algorithm::split(hosts, configured, boost::algorithm::is_any_of(";,"));
    for (std::string& h : hosts) {
        boost::algorithm::trim(h);
        boost::algorithm::to_lower(h);
    }
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [](const std::string& h) { return h.empty(); }), hosts.end());
    return hosts;
}

std::string host_of(const wxString& url)
{
    const wxURI uri(url);
    if (!uri.HasServer())
        return std::string();
    return std::string(uri.GetServer().Lower().ToUTF8().data());
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Impl
//
// Lifetime: Impl is held by shared_ptr so that an Http worker thread can keep it alive for the
// duration of a callback. The WebAppPanel it refers to has a *shorter* lifetime -- the panel owns
// the bridge -- so `owner` may only ever be touched on the GUI thread, after checking `detached`.
// Worker threads do local work and marshal everything else through wxGetApp().CallAfter.
// ---------------------------------------------------------------------------------------------

struct WebAppBridge::Impl : public std::enable_shared_from_this<WebAppBridge::Impl>
{
    explicit Impl(WebAppPanel& owner_) : owner(owner_) {}

    WebAppPanel&      owner;
    std::string       session_token;                // GUI thread only
    std::atomic<bool> detached{false};              // set by ~WebAppBridge on the GUI thread
    std::atomic<bool> download_busy{false};
    std::atomic<size_t> progress_mark{0};

    void send_envelope(const json& body);
    void send_response(const std::string& id, const std::string& method, int code, const std::string& message,
                       const json& data = json::object());
    void send_event(const std::string& id, const std::string& method, const json& data);

    void inject_bridge_script();

    // The method list slicer.getInfo reports and slicer.capabilitiesChanged re-reports.
    json capabilities() const;
    // Event delivery with the token and settings-opt-in checks; WebAppBridge::notify forwards here.
    void notify(const std::string& method, const json& data);

    void handle_get_info(const std::string& id);
    void handle_open_project(const std::string& id, const json& params);
    void handle_open_project_from_url(const std::string& id, const json& params);
    void handle_select_preset(const std::string& id, const json& params);

    // Printer-profile sharing (WEB_APP_BRIDGE.md section 4.4; policy in WebAppPresetShare.cpp).
    void handle_export_printer_preset(const std::string& id, const json& params);
    void handle_install_printer_preset(const std::string& id, const json& params);
    // From an accepted installPrinterPreset until its slicer.presetInstalled event is sent.
    std::atomic<bool> install_in_progress{false};

    // The download machinery openProjectFromUrl and installPrinterPreset share. The URL rules: http
    // or https, and the page's own host unless the slot's download-host list names it. Returns
    // WAB_OK, or the code and message to answer with.
    int  check_download_url(const std::string& url, std::string& message) const;
    // One download at a time per tab: claim the slot before answering "accepted", and on failure
    // answer 1007 instead.
    bool claim_download();
    struct DownloadJob
    {
        std::string id;
        std::string url;
        fs::path    destination;
        std::string expected_sha;   // lowercase hex; empty = not checked
        size_t      size_limit{WAB_MAX_DOWNLOAD_BYTES};
        std::string progress_event; // empty = no progress events
        // Worker thread, slot released, tab still alive: the request failed (network, HTTP status,
        // size limit). May only use what is thread-safe here -- send_event.
        std::function<void(Impl& self, const std::string& message, unsigned status)> on_http_error;
        // GUI thread, slot released, tab still alive: the body failed its checksum or could not be
        // written. Nothing is left at `destination`.
        std::function<void(Impl& self, const std::string& error)> on_save_error;
        // GUI thread, slot released, tab still alive: the verified file is at `destination`.
        std::function<void(Impl& self, const fs::path& destination)> on_ready;
    };
    // Runs `job` on an Http worker thread. The slot must have been claimed.
    void run_download(DownloadJob job);

    // Settings family (WEB_APP_BRIDGE.md section 4; policy in WebAppSettings.cpp).
    void handle_list_presets(const std::string& id, const json& params);
    void handle_get_settings_schema(const std::string& id, const json& params);
    void handle_get_settings(const std::string& id, const json& params);
    void handle_set_settings(const std::string& id, const json& params);
    void handle_get_plate(const std::string& id);
    void handle_export_project(const std::string& id, const json& params);
    void handle_read_export_chunk(const std::string& id, const json& params);
    void handle_discard_export(const std::string& id, const json& params);
    void handle_export_thumbnail(const std::string& id, const json& params);
    // Exports this bridge has handed out, by opaque handle. The page never learns or supplies a
    // path: it can only read back a file the slicer itself just wrote for it, which is what keeps
    // this from being the slicer.readFile the header forbids.
    std::map<std::string, boost::filesystem::path> exports;
    void discard_all_exports();
    void handle_slice(const std::string& id, const json& params);
    void handle_get_slice_result(const std::string& id, const json& params);

    // Common front matter of every settings-family call: the opt-in, "a dialog is open", "the
    // slicer is slicing" (writes only) and the tab for `type` (TYPE_INVALID: none needed).
    // Returns WAB_OK or the code to answer with; `message` is the reply text.
    // `require_settings` is the per-tab print-settings opt-in. The export family does its own
    // gate check against a DIFFERENT permission and passes false, so a tab allowed to publish the
    // plate is not silently refused for lacking a settings permission it never needed.
    int  preflight(bool write, Preset::Type type, Tab*& tab, std::string& message,
                   bool require_settings = true) const;
    // The ifDirty contract of every call that would replace the tab's edited preset: "ask"
    // (default) answers 1013 with the dirty keys, "discard" drops the edits, "prompt" leaves the
    // usual dialog to Tab::select_preset. `data` receives the 1013 payload.
    int  dirty_preflight(Tab* tab, const std::string& if_dirty, json& data, std::string& message) const;
    // Name or alias, "(modified)" suffix removed, to the installed preset's name. False if none.
    bool resolve_preset_name(PresetBundle& bundle, Preset::Type type, std::string& name) const;

    int  validate_local_path(const std::string& raw_path, fs::path& resolved, std::string& message);
    bool slicer_is_busy() const;
    void open_resolved_file(const std::string& id, const std::string& method, const fs::path& path);
};

// Thread-safe: builds the script from locals, then hops to the GUI thread to run it.
void WebAppBridge::Impl::send_envelope(const json& body)
{
    const wxString payload = wxString::FromUTF8(dump_json(body).c_str());
    const wxString script  = "(function(p){"
                             "  if (window.__slicerBridge && typeof window.__slicerBridge._recv === 'function')"
                             "    return window.__slicerBridge._recv(p);"
                             "  if (typeof HandleStudio === 'function') return HandleStudio(p);"
                             "  window.postMessage(p, window.location.origin);"
                             "})(" + payload + ");";

    std::weak_ptr<Impl> weak = weak_from_this();
    wxGetApp().CallAfter([weak, script]() {
        auto self = weak.lock();
        if (!self || self->detached.load())
            return;
        if (self->owner.browser() != nullptr)
            WebView::RunScript(self->owner.browser(), script);
    });
}

void WebAppBridge::Impl::send_response(const std::string& id, const std::string& method, int code,
                                       const std::string& message, const json& data)
{
    if (id.empty())
        return; // fire-and-forget request: nothing to correlate a response with

    json body       = json::object();
    body["type"]    = "response";
    body["v"]       = WAB_PROTOCOL_VERSION;
    body["id"]      = id;
    body["method"]  = method;
    body["code"]    = code;
    body["message"] = message;
    body["data"]    = data;
    send_envelope(body);
}

void WebAppBridge::Impl::send_event(const std::string& id, const std::string& method, const json& data)
{
    json body    = json::object();
    body["type"] = "event";
    body["v"]    = WAB_PROTOCOL_VERSION;
    if (!id.empty())
        body["id"] = id;
    body["method"] = method;
    body["data"]   = data;
    send_envelope(body);
}

void WebAppBridge::Impl::inject_bridge_script()
{
    wxWebView* browser = owner.browser();
    if (browser == nullptr || session_token.empty())
        return;

    // Injected with RunScript rather than AddUserScript on purpose: it lands only in the main
    // frame of a document whose origin we have already checked, and it means we never need
    // RemoveAllUserScripts(), which would also drop the "wx" script message handler.
    const wxString script = wxString::Format(
        "(function(){"
        "  var TOKEN=%s;"
        "  var seq=0, pending={}, listeners={};"
        "  function send(method, params){"
        "    return new Promise(function(resolve, reject){"
        "      if(!window.wx || typeof window.wx.postMessage!=='function'){"
        "        reject({code:'no-bridge', message:'not running inside the slicer'}); return;"
        "      }"
        "      var id='wa-'+(++seq);"
        "      var t=setTimeout(function(){ if(pending[id]){ delete pending[id];"
        "        reject({code:'timeout', message:'no response from the slicer', method:method}); } }, 60000);"
        "      pending[id]={resolve:resolve, reject:reject, timer:t};"
        "      window.wx.postMessage(JSON.stringify({v:1, id:id, method:method, token:TOKEN, params:params||{}}));"
        "    });"
        "  }"
        "  function emit(evt, data){"
        "    var fns=listeners[evt]||[];"
        "    for(var i=0;i<fns.length;i++){ try{ fns[i](data); }catch(e){} }"
        "  }"
        "  function recv(p){"
        "    if(!p) return;"
        "    if(typeof p==='string'){ try{ p=JSON.parse(p); }catch(e){ return; } }"
        "    if(p.type==='event'){ emit(p.method, p.data); return; }"
        "    var entry=pending[p.id];"
        "    if(!entry) return;"
        "    delete pending[p.id]; clearTimeout(entry.timer);"
        "    if(p.code===0) entry.resolve(p.data||{});"
        "    else entry.reject({code:p.code, message:p.message||'error', method:p.method, data:p.data||{}});"
        "  }"
        "  function on(evt, fn){ (listeners[evt]=listeners[evt]||[]).push(fn); }"
        "  function off(evt, fn){ var a=listeners[evt]||[]; var i=a.indexOf(fn); if(i>=0) a.splice(i,1); }"
        "  window.__slicerBridge=Object.freeze({version:1, token:TOKEN, send:send, on:on, off:off, _recv:recv});"
        "  try{ window.dispatchEvent(new Event('slicerbridgeready')); }catch(e){}"
        "})();",
        wxString::FromUTF8(json(session_token).dump().c_str()));

    WebView::RunScript(browser, script);
}

bool WebAppBridge::Impl::slicer_is_busy() const
{
    Plater* plater = wxGetApp().plater();
    return plater != nullptr && plater->is_background_process_slicing();
}

// ---------------------------------------------------------------------------------------------
// slicer.getInfo
// ---------------------------------------------------------------------------------------------

json WebAppBridge::Impl::capabilities() const
{
    const bool loopback    = owner.origin_is_loopback();
    AppConfig* cfg         = wxGetApp().app_config;
    const bool allow_local = loopback || (cfg != nullptr && cfg->get_bool(owner.slot().key_allow_local));

    // Computed at runtime so the page feature-detects rather than hard-coding what it may call.
    // With the settings opt-in off this is exactly the list protocol v1 shipped with.
    json capabilities = json::array({"slicer.getInfo", "slicer.openProjectFromUrl", "slicer.selectPreset"});
    if (allow_local)
        capabilities.push_back("slicer.openProject");
    if (owner.allows_settings())
        for (const char* method : {"slicer.listPresets", "slicer.getSettingsSchema", "slicer.getSettings", "slicer.setSettings",
                                   "slicer.getPlate", "slicer.slice", "slicer.getSliceResult"})
            capabilities.push_back(method);
    if (owner.allows_export())
        for (const char* method : {"slicer.exportProject", "slicer.readExportChunk", "slicer.discardExport",
                                   "slicer.exportThumbnail", "slicer.exportPrinterPreset"})
            capabilities.push_back(method);
    if (owner.allows_install())
        capabilities.push_back("slicer.installPrinterPreset");
    return capabilities;
}

void WebAppBridge::Impl::handle_get_info(const std::string& id)
{
    const WebAppSlot& slot = owner.slot();

    json types = json::array();
    for (const char* ext : WAB_FILE_TYPES)
        types.push_back(ext);

    json data    = json::object();
    data["api"]  = {{"version", WAB_PROTOCOL_VERSION}, {"envelope", "slicer-rpc/1"}};
    data["app"]  = {{"name", SLIC3R_APP_NAME},
                    {"version", UNLAYERED_VERSION},
                    {"coreVersion", SLIC3R_VERSION},
                    {"sourceUrl", WAB_SOURCE_URL},
                    {"license", "AGPL-3.0"}};
    data["platform"] = {{"os",
#if defined(_WIN32)
                         "windows"
#elif defined(__APPLE__)
                         "macos"
#else
                         "linux"
#endif
                        },
                        {"locale", std::string(wxGetApp().current_language_code().ToUTF8().data())},
                        {"darkMode", GUI_App::dark_mode()}};
    data["capabilities"] = capabilities();
    data["limits"]       = {{"maxDownloadBytes", WAB_MAX_DOWNLOAD_BYTES}, {"fileTypes", types}};
    data["origin"]       = {{"configured", std::string(WebAppPanel::configured_url(slot).ToUTF8().data())},
                            {"trusted", owner.origin_is_allowed()},
                            {"loopback", owner.origin_is_loopback()}};
    // Which of the tabs this bridge serves, and whether the settings family is open to it.
    data["tab"]            = {{"slot", slot.id}, {"title", std::string(WebAppPanel::configured_title(slot).ToUTF8().data())}};
    data["settingsAccess"] = owner.allows_settings();

    send_response(id, "slicer.getInfo", WAB_OK, "success", data);
}

// ---------------------------------------------------------------------------------------------
// Opening files
// ---------------------------------------------------------------------------------------------

int WebAppBridge::Impl::validate_local_path(const std::string& raw_path, fs::path& resolved, std::string& message)
{
    if (raw_path.empty() || raw_path.size() > 4096) {
        message = "path is empty or absurdly long";
        return WAB_BAD_PARAMS;
    }

#ifdef _WIN32
    // A UNC path would make the slicer authenticate to a server the page named, leaking an NTLM
    // hash to it. Never follow one on a page's say-so.
    if (raw_path.rfind("\\\\", 0) == 0 || raw_path.rfind("//", 0) == 0) {
        message = "UNC paths are not accepted";
        return WAB_FORBIDDEN_ORIGIN;
    }
#endif

    boost::system::error_code ec;
    resolved = fs::weakly_canonical(fs::absolute(path_from_utf8(raw_path)), ec);
    if (ec) {
        message = "path could not be resolved";
        return WAB_NOT_FOUND;
    }

    if (!fs::is_regular_file(resolved, ec) || ec) {
        message = "not a regular file";
        return WAB_NOT_FOUND;
    }

    const std::string ext = lowercase_extension(resolved);
    if (!extension_allowed(ext)) {
        message = "file type '" + ext + "' is not accepted";
        return WAB_UNSUPPORTED_TYPE;
    }

    const auto size = fs::file_size(resolved, ec);
    if (!ec && size > WAB_MAX_DOWNLOAD_BYTES) {
        message = "file is too large";
        return WAB_TOO_LARGE;
    }

    const std::vector<fs::path> roots  = allowed_path_roots(owner.slot());
    const bool                  inside = std::any_of(roots.begin(), roots.end(), [&resolved](const fs::path& root) {
        return path_is_under(resolved, root);
    });
    if (!inside) {
        message = "path is outside the folders this application may open from; serve the file over HTTP and use "
                  "slicer.openProjectFromUrl, or add the folder to " + std::string(owner.slot().key_path_roots);
        return WAB_FORBIDDEN_ORIGIN;
    }

    return WAB_OK;
}

// Reports slicer.projectOpened. Note precisely what ok:true means, because it is
// weaker than it looks: the file has been fetched, verified and handed to the
// slicer's own open path, which then runs asynchronously and may still put a
// "discard the current project?" modal in front of the user. There is no
// completion callback to hook -- request_open_project() and load_files() do not
// offer one -- so a user who cancels that modal still sees ok:true. Everything
// that can actually go wrong before that point (bad host, bad type, missing file,
// failed download, checksum mismatch, busy slicer) is reported accurately.
// WEB_APP_BRIDGE.md documents this limitation; do not let a client build on a
// stronger reading of it.
//
// G-code is its own case. Plater::load_files(std::vector<std::string>) is the MODEL
// import path and does not understand G-code at all -- only the wxArrayString
// drag-and-drop overload splits it off -- so a .gcode handed over the bridge used to
// fail even though fileTypes advertised it. Plater::load_gcode() is the right entry:
// it enters G-code-preview mode (m_only_gcode; sidebar and Slice disabled until a new
// project) and selects the Preview tab itself. Two of its behaviours reach the page:
//   - it goes through new_project(false, true), so a DIRTY project first shows the
//     "unsaved changes, save?" dialog (and dirty presets a second one); Cancel returns
//     silently, and per the paragraph above that still reports ok:true.
//   - it is a no-op when m_last_loaded_gcode == filename && m_only_gcode, i.e. a second
//     file with the SAME NAME is silently ignored. Clients must send a unique filename
//     per result (a re-slice of the same model included). WEB_APP_BRIDGE.md says so.
void WebAppBridge::Impl::open_resolved_file(const std::string& id, const std::string& method, const fs::path& path)
{
    const std::string utf8 = path_to_utf8(path);
    const std::string ext  = lowercase_extension(path);

    if (ext == "3mf") {
        // Inherits the "not allowed during slicing" guard and the recent-project plumbing.
        wxGetApp().request_open_project(utf8);
    } else if (ext == "gcode") {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", "no plater"}});
            return;
        }
        // Selects MainFrame::tpPreview on its own; no tab switch here.
        plater->load_gcode(from_path(path));
    } else {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", "no plater"}});
            return;
        }
        plater->load_files(std::vector<std::string>{utf8});
        if (wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->select_tab(static_cast<wxPanel*>(plater));
    }

    send_event(id, "slicer.projectOpened", {{"ok", true}, {"path", utf8}});
}

// ---------------------------------------------------------------------------------------------
// Selecting an installed preset
//
// A page that produces G-code for a particular printer needs the application to be ON that
// printer, or the preview draws the wrong bed and puts the toolpaths in the wrong place: the bed
// shape comes from the selected printer preset (Plater::set_bed_shape reads its printable_area),
// and loading G-code does not change the selection. So this switches the selection, and only
// that: an ALREADY-INSTALLED preset, named (or by alias), with no create, modify, delete or
// import.
//
// The PRINTER form is protocol v1's method and works without the settings opt-in; with the
// opt-in off it behaves exactly as it always has -- like the user picking the entry from the
// printer dropdown, "transfer or discard your changes?" dialog included. The PRINT and FILAMENT
// forms belong to the settings family: gated, and under the ifDirty contract (1013 by default)
// because a page cannot see the user's unsaved edits before it asks to replace them.
//
// The reply reports the selection actually in force afterwards rather than the one asked for.
void WebAppBridge::Impl::handle_select_preset(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.selectPreset";

    std::string type_text = json_string(params, "type");
    if (type_text.empty())
        type_text = "printer";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(type_text, type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"type\" must be \"printer\", \"print\" or \"filament\"");
        return;
    }
    std::string name = json_string(params, "name");
    if (name.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"name\" is required");
        return;
    }
    // Without the opt-in a page has no ifDirty: the printer form keeps its v1 dialog.
    const std::string if_dirty = owner.allows_settings() ? json_string(params, "ifDirty") : std::string("prompt");

    if (type == Preset::TYPE_PRINTER) {
        if (slicer_is_busy()) {
            send_response(id, METHOD, WAB_BUSY, "the slicer is busy slicing");
            return;
        }
        PresetBundle* bundle = wxGetApp().preset_bundle;
        Tab*          tab    = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (bundle == nullptr || tab == nullptr) {
            send_response(id, METHOD, WAB_INTERNAL, "no preset bundle");
            return;
        }
        const std::string current = bundle->printers.get_selected_preset_name();
        if (name != current && !resolve_preset_name(*bundle, type, name)) {
            send_response(id, METHOD, WAB_NOT_FOUND, "no installed printer preset is named \"" + name + "\"");
            return;
        }
        if (name == current) { // already there: answer without touching the selection at all
            send_response(id, METHOD, WAB_OK, "success", {{"type", "printer"}, {"selected", current}, {"changed", false}});
            return;
        }
        json        data;
        std::string message;
        if (const int code = dirty_preflight(tab, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
        tab->select_preset(name);
        const std::string now = bundle->printers.get_selected_preset_name();
        send_response(id, METHOD, WAB_OK, "success", {{"type", "printer"}, {"selected", now}, {"changed", now != current}});
        return;
    }

    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle& bundle    = *wxGetApp().preset_bundle;
    Plater*       plater    = wxGetApp().plater();
    const char*   type_name = WebAppSettings::preset_type_name(type);
    if (!resolve_preset_name(bundle, type, name)) {
        send_response(id, METHOD, WAB_NOT_FOUND, std::string("no installed ") + type_name + " preset is named \"" + name + "\"");
        return;
    }

    if (type == Preset::TYPE_PRINT) {
        const std::string current = bundle.prints.get_selected_preset_name();
        if (name == current) {
            send_response(id, METHOD, WAB_OK, "success", {{"type", "print"}, {"selected", current}, {"changed", false}});
            return;
        }
        json data;
        if (const int code = dirty_preflight(tab, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
        tab->select_preset(name);
        plater->on_config_change(bundle.full_config()); // as Plater::priv::on_select_preset does after the tab
        const std::string now = bundle.prints.get_selected_preset_name();
        send_response(id, METHOD, WAB_OK, "success", {{"type", "print"}, {"selected", now}, {"changed", now != current}});
        return;
    }

    // Filament: one slot of the sidebar, through the sequence the sidebar combo runs.
    const int slot = json_int(params, "slot", 0);
    if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
        return;
    }
    const std::string current = bundle.filament_presets[slot];
    if (name == current) {
        send_response(id, METHOD, WAB_OK, "success", {{"type", "filament"}, {"slot", slot}, {"selected", current}, {"changed", false}});
        return;
    }
    if (!plater->sidebar().is_multifilament()) {
        // A single-filament printer routes the pick through the filament tab, which owns the
        // dirty edits; a multi-filament one only renames the slot.
        json data;
        if (const int code = dirty_preflight(tab, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
    }
    plater->select_filament_preset(static_cast<size_t>(slot), name);
    const std::string now = bundle.filament_presets[slot];
    send_response(id, METHOD, WAB_OK, "success", {{"type", "filament"}, {"slot", slot}, {"selected", now}, {"changed", now != current}});
}

void WebAppBridge::Impl::handle_open_project(const std::string& id, const json& params)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!owner.origin_is_loopback() && (cfg == nullptr || !cfg->get_bool(owner.slot().key_allow_local))) {
        send_response(id, "slicer.openProject", WAB_CAPABILITY_DISABLED,
                      "opening local files is available to loopback applications only, unless " +
                          std::string(owner.slot().key_allow_local) + " is enabled");
        return;
    }

    if (slicer_is_busy()) {
        send_response(id, "slicer.openProject", WAB_BUSY, "the slicer is busy slicing");
        return;
    }

    fs::path    resolved;
    std::string message;
    const int   code = validate_local_path(json_string(params, "path"), resolved, message);
    if (code != WAB_OK) {
        send_response(id, "slicer.openProject", code, message);
        return;
    }

    // Accepted, not finished: opening may put a modal in front of the user. The outcome arrives
    // as a slicer.projectOpened event carrying the same id.
    send_response(id, "slicer.openProject", WAB_OK, "accepted");
    open_resolved_file(id, "slicer.openProject", resolved);
}

int WebAppBridge::Impl::check_download_url(const std::string& url, std::string& message) const
{
    const wxString wx_url = wxString::FromUTF8(url.c_str());
    const wxURI    uri(wx_url);
    const wxString scheme = uri.HasScheme() ? uri.GetScheme().Lower() : wxString();
    if (scheme != "http" && scheme != "https") {
        message = "only http and https URLs are accepted";
        return WAB_BAD_PARAMS;
    }

    // Same host as the page by default; anything else must be named in web_app_download_hosts.
    const std::string url_host  = host_of(wx_url);
    const std::string page_host = host_of(owner.current_origin());
    if (url_host.empty()) {
        message = "url has no host";
        return WAB_BAD_PARAMS;
    }
    if (url_host != page_host) {
        const std::vector<std::string> extra = configured_download_hosts(owner.slot());
        // An entry beginning with a dot matches the whole family: ".example.com" accepts
        // "a.example.com" and "example.com" itself, but never "notexample.com". Exact hosts stay
        // exact. Sub-domain matching exists because object storage hands out a host that belongs
        // to the deployment rather than to the product, so no fixed name can be shipped.
        const bool allowed = std::any_of(extra.begin(), extra.end(), [&url_host](const std::string& h) {
            if (h.empty())
                return false;
            if (h.front() != '.')
                return h == url_host;
            if (url_host.size() < h.size())
                return url_host == h.substr(1);
            return url_host.compare(url_host.size() - h.size(), h.size(), h) == 0;
        });
        if (!allowed) {
            message = "downloads must come from the application's own host unless that host is listed in " +
                      std::string(owner.slot().key_download_hosts);
            return WAB_FORBIDDEN_ORIGIN;
        }
    }
    return WAB_OK;
}

bool WebAppBridge::Impl::claim_download()
{
    bool expected = false;
    if (!download_busy.compare_exchange_strong(expected, true))
        return false;
    progress_mark.store(0);
    return true;
}

void WebAppBridge::Impl::run_download(DownloadJob job)
{
    // Everything below runs on an Http worker thread. It may only touch Impl through the weak
    // pointer, and must never touch Impl::owner -- that is the GUI thread's alone.
    std::weak_ptr<Impl> weak = weak_from_this();
    const std::string   id   = job.id;
    // Shared by the callbacks, which each get a copy of the closure.
    auto shared = std::make_shared<DownloadJob>(std::move(job));

    Http::get(shared->url)
        .size_limit(shared->size_limit)
        .timeout_connect(10)
        .on_progress([weak, id, shared](Http::Progress progress, bool& cancel) {
            auto self = weak.lock();
            if (!self || self->detached.load()) {
                cancel = true;
                return;
            }
            if (shared->progress_event.empty())
                return;
            // One event per MiB, plus the final one: on_progress fires far too often to forward
            // every tick into the GUI thread's event queue.
            const size_t step = progress.dlnow / WAB_PROGRESS_STEP;
            if (step == self->progress_mark.load() && !(progress.dltotal > 0 && progress.dlnow >= progress.dltotal))
                return;
            self->progress_mark.store(step);
            self->send_event(id, shared->progress_event, {{"received", progress.dlnow}, {"total", progress.dltotal}});
        })
        .on_error([weak, shared](std::string /*body*/, std::string error, unsigned status) {
            auto self = weak.lock();
            if (!self)
                return;
            self->download_busy.store(false);
            const std::string message = error.empty() ? ("HTTP " + std::to_string(status)) : error;
            BOOST_LOG_TRIVIAL(info) << "WebAppBridge: download failed: " << message;
            if (!self->detached.load())
                shared->on_http_error(*self, message, status);
        })
        .on_complete([weak, shared](std::string body, unsigned /*status*/) {
            auto self = weak.lock();
            if (!self)
                return;

            const fs::path&    destination  = shared->destination;
            const std::string& expected_sha = shared->expected_sha;
            std::string        error;
            if (!expected_sha.empty()) {
                const std::vector<uint8_t> bytes(body.begin(), body.end());
                const std::string          actual = sha256_hex(bytes);
                if (!boost::algorithm::iequals(actual, expected_sha))
                    error = "checksum mismatch: expected " + expected_sha + ", got " + actual;
            }

            if (error.empty()) {
                boost::system::error_code ec;
                fs::create_directories(destination.parent_path(), ec);
                fs::ofstream out(destination, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out.good()) {
                    error = "could not write " + path_to_utf8(destination);
                } else {
                    out.write(body.data(), static_cast<std::streamsize>(body.size()));
                    out.close();
                }
            }

            if (!error.empty()) {
                boost::system::error_code ec;
                fs::remove(destination, ec); // never leave a partial or unverified file behind
                BOOST_LOG_TRIVIAL(info) << "WebAppBridge: " << error;
            }

            wxGetApp().CallAfter([weak, shared, error]() {
                auto self = weak.lock();
                if (!self)
                    return;
                self->download_busy.store(false);
                if (self->detached.load())
                    return;
                if (!error.empty()) {
                    shared->on_save_error(*self, error);
                    return;
                }
                shared->on_ready(*self, shared->destination);
            });
        })
        .perform();
}

void WebAppBridge::Impl::handle_open_project_from_url(const std::string& id, const json& params)
{
    const std::string url = json_string(params, "url");
    if (url.empty()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BAD_PARAMS, "url is required");
        return;
    }

    std::string url_message;
    if (const int code = check_download_url(url, url_message); code != WAB_OK) {
        send_response(id, "slicer.openProjectFromUrl", code, url_message);
        return;
    }

    std::string name = sanitise_filename(json_string(params, "filename"));
    if (name.empty())
        name = sanitise_filename(Http::get_filename_from_url(url));
    if (name.empty()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BAD_PARAMS, "filename is required");
        return;
    }
    const std::string ext = lowercase_extension(path_from_utf8(name));
    if (!extension_allowed(ext)) {
        send_response(id, "slicer.openProjectFromUrl", WAB_UNSUPPORTED_TYPE, "file type '" + ext + "' is not accepted");
        return;
    }

    if (slicer_is_busy()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BUSY, "the slicer is busy slicing");
        return;
    }

    if (!claim_download()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BUSY, "another download is already in progress");
        return;
    }

    DownloadJob job;
    job.id             = id;
    job.url            = url;
    job.expected_sha   = boost::algorithm::to_lower_copy(json_string(params, "sha256"));
    job.destination    = webapp_cache_dir() / path_from_utf8(name);
    job.size_limit     = WAB_MAX_DOWNLOAD_BYTES;
    job.progress_event = "slicer.openProjectFromUrl.progress";
    job.on_http_error  = [id](Impl& self, const std::string& message, unsigned status) {
        const int code = (status == 404 || status == 410) ? WAB_NOT_FOUND : WAB_IO;
        self.send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", message}, {"code", code}});
    };
    job.on_save_error = [id](Impl& self, const std::string& error) {
        self.send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", error}});
    };
    job.on_ready = [id](Impl& self, const fs::path& destination) {
        self.open_resolved_file(id, "slicer.openProjectFromUrl", destination);
    };

    send_response(id, "slicer.openProjectFromUrl", WAB_OK, "accepted");
    run_download(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Preset settings (WEB_APP_BRIDGE.md section 4; policy in WebAppSettings.cpp)
//
// Eight methods behind one per-tab opt-in, all on the GUI thread, all through preflight(). The
// write path is Tab::load_config(), the entry the cut-plane gizmo uses: a value becomes an
// ordinary unsaved edit of the tab's preset, nothing touches disk, and Undo works.
// ---------------------------------------------------------------------------------------------

namespace {

bool json_bool(const json& node, const char* key, bool fallback)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

int json_int(const json& node, const char* key, int fallback)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_number()) ? static_cast<int>(it->get<double>()) : fallback;
}

// Events a page sees only with the settings opt-in: they describe the user's presets and plate.
bool is_settings_event(const std::string& method)
{
    return method == "slicer.presetChanged" || method == "slicer.settingsChanged" || method == "slicer.plateChanged" ||
           method == "slicer.sliceStarted" || method == "slicer.sliceFinished";
}

PresetCollection* collection_for(PresetBundle& bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return &bundle.prints;
    case Preset::TYPE_FILAMENT: return &bundle.filaments;
    case Preset::TYPE_PRINTER:  return &bundle.printers;
    default:                    return nullptr;
    }
}

// A page may send a string, a number, a bool or an array of those; set_deserialize() wants the
// option's .ini form. Strings pass through untouched, so "35%" and "0.2" both work as typed.
bool value_to_ini_string(const json& value, const ConfigOptionDef& def, std::string& out)
{
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_boolean()) {
        out = value.get<bool>() ? "1" : "0";
        return true;
    }
    if (value.is_number()) {
        out = value.dump();
        return true;
    }
    if (value.is_array()) {
        if (def.type == coStrings) {
            std::vector<std::string> items;
            for (const json& item : value) {
                if (!item.is_string())
                    return false;
                items.push_back(item.get<std::string>());
            }
            out = escape_strings_cstyle(items);
            return true;
        }
        std::string joined;
        for (const json& item : value) {
            std::string text;
            if (item.is_array() || item.is_object() || !value_to_ini_string(item, def, text))
                return false;
            if (!joined.empty())
                joined += ',';
            joined += text;
        }
        out = joined;
        return true;
    }
    return false;
}

// ConfigOptionDef::is_value_valid() for the numeric types, scalar and vector. Other types are
// accepted as set_deserialize() left them; a percentage in a float-or-percent is not
// range-checked, as the GUI field does not check it either.
bool numeric_value_valid(const ConfigOptionDef& def, const ConfigOption* opt)
{
    if (opt == nullptr)
        return true;
    switch (def.type) {
    case coFloat:
    case coPercent:
        return def.is_value_valid(opt->getFloat());
    case coFloatOrPercent:
        if (auto* fp = dynamic_cast<const ConfigOptionFloatOrPercent*>(opt); fp != nullptr && fp->percent)
            return true;
        return def.is_value_valid(opt->getFloat());
    case coInt:
        return def.is_value_valid(static_cast<double>(opt->getInt()));
    case coFloats:
    case coPercents:
        if (def.nullable)
            return true;
        if (auto* floats = dynamic_cast<const ConfigOptionFloats*>(opt); floats != nullptr)
            for (double v : floats->values)
                if (!def.is_value_valid(v))
                    return false;
        return true;
    case coInts:
        if (def.nullable)
            return true;
        if (auto* ints = dynamic_cast<const ConfigOptionInts*>(opt); ints != nullptr)
            for (int v : ints->values)
                if (!def.is_value_valid(static_cast<double>(v)))
                    return false;
        return true;
    default:
        return true;
    }
}

std::string range_text(const ConfigOptionDef& def)
{
    std::ostringstream oss;
    oss << "value out of range [";
    if (def.min > -FLT_MAX)
        oss << def.min;
    else
        oss << "-inf";
    oss << ", ";
    if (def.max < FLT_MAX)
        oss << def.max;
    else
        oss << "inf";
    oss << "]";
    return oss.str();
}

const char* TYPE_PARAM_ERROR = "\"type\" must be \"printer\", \"print\" or \"filament\"";

} // namespace

void WebAppBridge::Impl::notify(const std::string& method, const json& data)
{
    if (session_token.empty())
        return; // no trusted document loaded: nobody to tell
    if (is_settings_event(method) && !owner.allows_settings())
        return;
    send_event("", method, data);
}

int WebAppBridge::Impl::preflight(bool write, Preset::Type type, Tab*& tab, std::string& message,
                                  bool require_settings) const
{
    tab = nullptr;
    if (require_settings && !owner.allows_settings()) {
        message = "print settings access is not enabled for this tab";
        return WAB_CAPABILITY_DISABLED;
    }
    // A modal dialog disables the frame; changing presets under one would nest a second modal
    // inside it. A heuristic against a bad experience, not a security check.
    MainFrame* mainframe = wxGetApp().mainframe;
    if (mainframe == nullptr || !mainframe->IsEnabled()) {
        message = "a dialog is open";
        return WAB_BUSY;
    }
    if (write && slicer_is_busy()) {
        message = "the slicer is busy slicing";
        return WAB_BUSY;
    }
    if (wxGetApp().preset_bundle == nullptr || wxGetApp().plater() == nullptr) {
        message = "no preset bundle";
        return WAB_INTERNAL;
    }
    if (type != Preset::TYPE_INVALID) {
        tab = wxGetApp().get_tab(type);
        if (tab == nullptr) {
            message = "no settings tab for this preset type";
            return WAB_INTERNAL;
        }
    }
    return WAB_OK;
}

int WebAppBridge::Impl::dirty_preflight(Tab* tab, const std::string& if_dirty, json& data, std::string& message) const
{
    PresetCollection* presets = tab->get_presets();
    if (presets == nullptr || !presets->current_is_dirty())
        return WAB_OK;
    if (if_dirty == "prompt")
        return WAB_OK; // Tab::select_preset shows the usual transfer/discard dialog
    if (if_dirty == "discard") {
        presets->discard_current_changes();
        tab->update_dirty();
        tab->reload_config();
        return WAB_OK;
    }
    if (if_dirty.empty() || if_dirty == "ask") {
        const char* type = WebAppSettings::preset_type_name(tab->type());
        data    = {{"dirty", true}, {"type", type}, {"dirtyKeys", presets->current_dirty_options()}};
        message = std::string("the edited ") + type +
                  " preset has unsaved changes; retry with ifDirty:\"discard\" to drop them, or ifDirty:\"prompt\" to let the user decide";
        return WAB_NEEDS_CONFIRMATION;
    }
    message = "ifDirty must be \"ask\", \"discard\" or \"prompt\"";
    return WAB_BAD_PARAMS;
}

bool WebAppBridge::Impl::resolve_preset_name(PresetBundle& bundle, Preset::Type type, std::string& name) const
{
    PresetCollection* collection = collection_for(bundle, type);
    if (collection == nullptr)
        return false;
    name = Preset::remove_suffix_modified(name);
    if (collection->find_preset(name) != nullptr)
        return true;
    const std::string by_alias = bundle.get_preset_name_by_alias(type, name);
    if (by_alias != name && collection->find_preset(by_alias) != nullptr) {
        name = by_alias;
        return true;
    }
    return false;
}

void WebAppBridge::Impl::handle_list_presets(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.listPresets";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    PresetBundle&     bundle          = *wxGetApp().preset_bundle;
    PresetCollection& collection      = *collection_for(bundle, type);
    const std::string query           = json_string(params, "query");
    const bool        compatible_only = json_bool(params, "compatibleOnly", true);
    const size_t      limit           = static_cast<size_t>(std::clamp(json_int(params, "limit", 500), 1, 5000));

    json   presets = json::array();
    size_t total   = 0;
    for (auto it = collection.begin(); it != collection.end(); ++it) { // begin() skips "- default -"
        const Preset& preset = *it;
        if (!preset.is_visible)
            continue;
        if (compatible_only && !preset.is_compatible)
            continue;
        if (!query.empty() && !boost::algorithm::icontains(preset.name, query) && !boost::algorithm::icontains(preset.alias, query))
            continue;
        ++total;
        if (presets.size() < limit)
            presets.push_back({{"name", preset.name},
                               {"alias", preset.alias},
                               {"isSystem", preset.is_system},
                               {"isUser", preset.is_user()},
                               {"isCompatible", preset.is_compatible},
                               {"isVisible", preset.is_visible}});
    }

    json data = {{"type", WebAppSettings::preset_type_name(type)},
                 {"selected", collection.get_selected_preset_name()},
                 {"edited", {{"name", collection.get_edited_preset().name}, {"dirty", collection.current_is_dirty()}}},
                 {"total", total},
                 {"presets", presets}};
    if (type == Preset::TYPE_FILAMENT) {
        data["slots"] = bundle.filament_presets;
        if (auto* colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour"); colours != nullptr)
            data["slotColours"] = colours->values;
    }
    send_response(id, METHOD, WAB_OK, "success", data);
}

void WebAppBridge::Impl::handle_get_settings_schema(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSettingsSchema";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    const bool                include_code = json_bool(params, "includeCode", false);
    const size_t              limit        = static_cast<size_t>(std::clamp(json_int(params, "limit", 200), 1, 5000));
    const std::string         category     = json_string(params, "category");
    const std::string         query        = json_string(params, "query");
    const DynamicPrintConfig* cfg          = tab->get_config();

    std::vector<std::string> keys;
    if (auto it = params.find("keys"); it != params.end() && it->is_array()) {
        for (const json& item : *it) {
            if (!item.is_string()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "\"keys\" must be an array of option names");
                return;
            }
            const std::string key = item.get<std::string>();
            if (print_config_def.get(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "unknown option \"" + key + "\"");
                return;
            }
            keys.push_back(key);
        }
    } else {
        keys = cfg->keys(); // what this tab's preset actually holds
    }

    json   options = json::array();
    size_t total   = 0;
    for (const std::string& key : keys) {
        if (!WebAppSettings::key_readable(key, include_code))
            continue;
        const ConfigOptionDef* def = print_config_def.get(key);
        if (!category.empty() && !boost::algorithm::iequals(def->category, category))
            continue;
        if (!query.empty() && !boost::algorithm::icontains(key, query) && !boost::algorithm::icontains(def->label, query) &&
            !boost::algorithm::icontains(def->full_label, query) && !boost::algorithm::icontains(def->tooltip, query))
            continue;
        ++total;
        if (options.size() < limit)
            options.push_back(WebAppSettings::option_schema_json(key, cfg));
    }
    send_response(id, METHOD, WAB_OK, "success",
                  {{"type", WebAppSettings::preset_type_name(type)},
                   {"total", total},
                   {"truncated", total > options.size()},
                   {"options", options}});
}

void WebAppBridge::Impl::handle_get_settings(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSettings";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    const bool                include_code = json_bool(params, "includeCode", false);
    const char*               type_name    = WebAppSettings::preset_type_name(type);
    PresetBundle&             bundle       = *wxGetApp().preset_bundle;
    PresetCollection&         collection   = *tab->get_presets();
    const DynamicPrintConfig* cfg          = tab->get_config();
    std::string               source       = collection.get_edited_preset().name;
    int                       slot         = -1;

    if (type == Preset::TYPE_FILAMENT && params.contains("slot")) {
        slot = json_int(params, "slot", -1);
        if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
            return;
        }
        const std::string& slot_name = bundle.filament_presets[slot];
        if (slot_name != source) {
            // Another slot's preset: its saved values. Only the tab's own preset carries edits.
            const Preset* preset = collection.find_preset(slot_name);
            if (preset == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "the preset of slot " + std::to_string(slot) + " is not installed");
                return;
            }
            cfg    = &preset->config;
            source = slot_name;
        }
    }

    json values = json::object();
    if (auto it = params.find("keys"); it != params.end() && it->is_array()) {
        for (const json& item : *it) {
            if (!item.is_string()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "\"keys\" must be an array of option names");
                return;
            }
            const std::string key = item.get<std::string>();
            if (print_config_def.get(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "unknown option \"" + key + "\"");
                return;
            }
            if (!WebAppSettings::key_readable(key, include_code)) {
                send_response(id, METHOD, WAB_FORBIDDEN_ORIGIN, "\"" + key + "\" is not readable over the bridge");
                return;
            }
            if (cfg->option(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "\"" + key + "\" is not a " + type_name + " setting");
                return;
            }
            values[key] = cfg->opt_serialize(key);
        }
    } else {
        for (const std::string& key : cfg->keys())
            if (WebAppSettings::key_readable(key, include_code))
                values[key] = cfg->opt_serialize(key);
    }

    json preset = {{"selected", collection.get_selected_preset_name()},
                   {"edited", collection.get_edited_preset().name},
                   {"dirty", collection.current_is_dirty()},
                   {"dirtyKeys", collection.current_dirty_options()},
                   {"source", source}};
    if (slot >= 0)
        preset["slot"] = slot;
    send_response(id, METHOD, WAB_OK, "success", {{"type", type_name}, {"preset", preset}, {"values", values}});
}

void WebAppBridge::Impl::handle_set_settings(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setSettings";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    const auto values_it = params.find("values");
    if (values_it == params.end() || !values_it->is_object() || values_it->empty() || values_it->size() > 64) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"values\" must be an object with 1 to 64 entries");
        return;
    }

    const char*       type_name  = WebAppSettings::preset_type_name(type);
    PresetBundle&     bundle     = *wxGetApp().preset_bundle;
    PresetCollection& collection = *tab->get_presets();

    // A filament slot names a preset; to edit that preset the filament tab must be on it. Getting
    // there is a preset switch, so the user's dirty edits get the same ifDirty protection as
    // slicer.selectPreset.
    if (type == Preset::TYPE_FILAMENT && params.contains("slot")) {
        const int slot = json_int(params, "slot", -1);
        if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
            return;
        }
        const std::string slot_name = bundle.filament_presets[slot];
        if (slot_name != collection.get_edited_preset().name) {
            if (collection.find_preset(slot_name) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "the preset of slot " + std::to_string(slot) + " is not installed");
                return;
            }
            json data;
            if (const int code = dirty_preflight(tab, json_string(params, "ifDirty"), data, message); code != WAB_OK) {
                send_response(id, METHOD, code, message, data);
                return;
            }
            tab->select_preset(slot_name);
            if (collection.get_edited_preset().name != slot_name) {
                send_response(id, METHOD, WAB_USER_CANCELLED, "the user kept the current preset",
                              {{"selected", collection.get_edited_preset().name}});
                return;
            }
        }
    }

    const DynamicPrintConfig& original = *tab->get_config();
    DynamicPrintConfig        copy     = original;
    std::vector<std::string>  applied;
    json                      rejected = json::array();
    auto reject = [&rejected](const std::string& key, int code, const std::string& why) {
        rejected.push_back({{"key", key}, {"code", code}, {"message", why}});
    };

    for (auto it = values_it->begin(); it != values_it->end(); ++it) {
        const std::string&     key = it.key();
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr) {
            reject(key, WAB_NOT_FOUND, "unknown option");
            continue;
        }
        std::string why;
        if (!WebAppSettings::key_writable(key, why)) {
            reject(key, WAB_FORBIDDEN_ORIGIN, why);
            continue;
        }
        const ConfigOption* before = original.option(key);
        if (before == nullptr) {
            reject(key, WAB_BAD_PARAMS, std::string("not a ") + type_name + " setting");
            continue;
        }
        std::string text;
        if (!value_to_ini_string(it.value(), *def, text)) {
            reject(key, WAB_BAD_PARAMS, "value must be a string, number, bool or an array of those");
            continue;
        }
        std::string error;
        try {
            ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
            copy.set_deserialize(key, text, context); // runs handle_legacy(); throws on a value it cannot parse
            if (!numeric_value_valid(*def, copy.option(key)))
                error = range_text(*def);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!error.empty()) {
            copy.set_key_value(key, before->clone()); // undo whatever a failed parse left behind
            reject(key, WAB_BAD_PARAMS, error);
            continue;
        }
        applied.push_back(key);
    }

    if (applied.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "no value was applied", {{"applied", json::array()}, {"rejected", rejected}});
        return;
    }

    {
        // Tab::on_value_change must not echo these back as user edits; the event below is the one
        // report of this write.
        WebAppBridgeWriteScope write_scope;
        tab->load_config(copy); // update_dirty + reload_config + update: the sidebar path

        // The couplings Tab::on_value_change keeps for the GUI's field handlers; load_config does
        // not run them. Each wants the boost::any the field would have produced (Tab.cpp casts).
        bool coupled = false;
        for (const std::string& key : applied) {
            if (key == "pellet_flow_coefficient" || key == "filament_diameter") {
                if (auto* floats = dynamic_cast<const ConfigOptionFloats*>(copy.option(key)); floats != nullptr && !floats->values.empty()) {
                    tab->on_value_change(key, boost::any(floats->values.front()));
                    coupled = true;
                }
            } else if (key == "enable_prime_tower" || key == "single_extruder_multi_material" || key == "purge_in_prime_tower") {
                if (auto* flag = dynamic_cast<const ConfigOptionBool*>(copy.option(key)); flag != nullptr) {
                    tab->on_value_change(key, boost::any(flag->value));
                    coupled = true;
                }
            } else if (key == "gcode_flavor") {
                tab->on_value_change(key, boost::any());
                coupled = true;
            }
        }
        if (coupled) {
            tab->update_dirty();
            tab->reload_config();
        }
    }

    send_response(id, METHOD, WAB_OK, "success",
                  {{"applied", applied},
                   {"rejected", rejected},
                   {"preset", {{"type", type_name}, {"selected", collection.get_selected_preset_name()}, {"dirty", collection.current_is_dirty()}}}});
    notify("slicer.settingsChanged", {{"type", type_name}, {"keys", applied}, {"source", "bridge"}});
}

void WebAppBridge::Impl::handle_get_plate(const std::string& id)
{
    static const char* METHOD = "slicer.getPlate";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater*        plater  = wxGetApp().plater();
    PartPlateList& plates  = plater->get_partplate_list();
    const Model&   model   = plater->model();
    const int      current = plates.get_curr_plate_index();

    json plate_list = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;
        plate_list.push_back({{"index", i},
                              {"isCurrent", i == current},
                              {"name", plate->get_plate_name()},
                              {"hasPrintableInstances", plate->has_printable_instances()},
                              {"sliceResultValid", plate->is_slice_result_valid()}});
    }

    constexpr size_t max_objects = 100;
    json             objects     = json::array();
    for (size_t obj_idx = 0; obj_idx < model.objects.size() && objects.size() < max_objects; ++obj_idx) {
        const ModelObject* object   = model.objects[obj_idx];
        int                on_plate = -1;
        for (int i = 0; i < plates.get_plate_count() && on_plate < 0; ++i)
            if (PartPlate* plate = plates.get_plate(i); plate != nullptr && plate->contain_instance(int(obj_idx), 0))
                on_plate = i;
        json entry = {{"index", obj_idx},
                      {"name", object->name},
                      {"plate", on_plate},
                      {"printable", object->printable},
                      {"instances", object->instances.size()}};
        if (!object->instances.empty()) {
            const BoundingBoxf3 bb   = object->instance_bounding_box(0);
            const Vec3d         size = bb.size();
            entry["bboxMm"] = {{"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                               {"max", {bb.max.x(), bb.max.y(), bb.max.z()}},
                               {"size", {size.x(), size.y(), size.z()}}};
        }
        objects.push_back(entry);
    }

    int    width = 0, depth = 0;
    double height = 0.;
    plates.get_plate_size(width, depth, height);

    send_response(id, METHOD, WAB_OK, "success",
                  {{"printerTechnology", plater->printer_technology() == ptSLA ? "SLA" : "FFF"},
                   {"projectName", std::string(plater->get_project_name().ToUTF8().data())},
                   {"projectDirty", plater->is_project_dirty()},
                   {"isEmpty", model.objects.empty()},
                   {"currentPlate", current},
                   {"plateCount", plates.get_plate_count()},
                   {"bedMm", {{"width", width}, {"depth", depth}, {"height", height}}},
                   {"plates", plate_list},
                   {"objects", objects},
                   {"objectsTruncated", model.objects.size() > max_objects}});
}

// How much of an export the page may pull in one call. Base64 inflates by a third and the whole
// chunk crosses the process boundary as a single JSON string, so this is a latency budget rather
// than a capacity one: big enough that a typical project is a handful of round trips, small enough
// that none of them visibly stalls the GUI thread.
constexpr size_t WAB_EXPORT_CHUNK_BYTES = 4u * 1024u * 1024u;
// At most this many exports alive at once per tab, so a page that never calls discardExport cannot
// quietly fill the cache directory.
constexpr size_t WAB_MAX_LIVE_EXPORTS = 3;

void WebAppBridge::Impl::discard_all_exports()
{
    boost::system::error_code ec;
    for (const auto& entry : exports)
        fs::remove(entry.second, ec);
    exports.clear();
}

void WebAppBridge::Impl::handle_export_project(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.exportProject";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater* plater = wxGetApp().plater();
    if (plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (exports.size() >= WAB_MAX_LIVE_EXPORTS) {
        send_response(id, METHOD, WAB_BUSY,
                      "too many exports are still open; discard the ones you have finished with");
        return;
    }

    // One plate, or the whole project. -1 is what export_3mf reads as "everything".
    int plate_idx = -1;
    if (params.contains("plate")) {
        PartPlateList& plates = plater->get_partplate_list();
        plate_idx             = json_int(params, "plate", -1);
        if (plate_idx < 0 || plate_idx >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }

    // Named after the project, so what the user publishes carries the name they gave it, plus a
    // random part because several exports can be alive at once.
    std::string stem = sanitise_filename(std::string(plater->get_project_name().ToUTF8().data()));
    if (stem.empty())
        stem = "project";
    const std::string handle   = make_session_token().substr(0, 16);
    const fs::path    out_path = webapp_cache_dir() / path_from_utf8(stem + "-" + handle + ".3mf");

    // 0.1.6 privacy: handing the project to a web page is a model upload, so it is asked every time,
    // for this file, with what / why / to whom / how long (PrivacyConsent::ModelUploadRequest). Usage
    // diagnostics consent never covers it, and a "no" changes nothing else in the slicer. The page
    // learns whether AI training was allowed for this upload and must keep to it.
    PrivacyConsent::ModelUploadRequest upload;
    upload.file_label    = stem + ".3mf";
    upload.file_id       = handle;
    upload.included_data = plate_idx < 0 ? _u8L("The whole project as a 3MF file: every model's geometry, the settings, and plate thumbnails.")
                                         : _u8L("This plate as a 3MF file: its models' geometry, the settings, and the plate thumbnail.");
    upload.purpose       = _u8L("Sharing or publishing it on Unlayered Universe, as you asked in the Universe tab.");
    upload.ai_training_proposed = true;
    upload.recipients    = _u8L("Unlayered 3D, Inc., through Unlayered Universe (unlayered3d.com).");
    upload.retention     = _u8L("As set out in the Unlayered Universe terms and privacy policy; you can delete what you publish there.");
    PrivacyConsent::ModelUploadAuthorization authorization = ModelUploadConsentDialog::ask(wxGetApp().mainframe, upload);
    const bool ai_training = authorization.ai_training_allowed();
    if (!authorization.consume(handle)) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "the user declined to upload this project");
        return;
    }

    boost::system::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);

    // Silence: this runs behind a click in the page, so it must not raise a dialog of its own.
    if (plater->export_3mf(out_path, SaveStrategy::Silence, plate_idx) < 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the slicer could not export the project");
        return;
    }

    const std::uintmax_t size = fs::file_size(out_path, ec);
    if (ec || size == 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the export produced no file");
        return;
    }
    if (size > WAB_MAX_DOWNLOAD_BYTES) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "the exported project is larger than the " +
                          std::to_string(WAB_MAX_DOWNLOAD_BYTES / (1024 * 1024)) + " MB limit");
        return;
    }

    exports[handle] = out_path;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"filename", stem + ".3mf"},
                   {"sizeBytes", size},
                   {"chunkBytes", WAB_EXPORT_CHUNK_BYTES},
                   // The person's answer for this upload; false unless they ticked the box.
                   {"aiTrainingAllowed", ai_training}});
}

void WebAppBridge::Impl::handle_read_export_chunk(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.readExportChunk";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }

    const std::string handle = json_string(params, "handle");
    const auto        it     = exports.find(handle);
    if (handle.empty() || it == exports.end()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "unknown export handle");
        return;
    }

    boost::system::error_code ec;
    const std::uintmax_t      size = fs::file_size(it->second, ec);
    if (ec) {
        send_response(id, METHOD, WAB_INTERNAL, "the export is gone");
        return;
    }

    const long long offset = params.contains("offset") ? json_int(params, "offset", 0) : 0;
    if (offset < 0 || std::uintmax_t(offset) > size) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "offset is outside the export");
        return;
    }
    size_t want = WAB_EXPORT_CHUNK_BYTES;
    if (params.contains("length")) {
        const long long asked = json_int(params, "length", 0);
        if (asked <= 0) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "length must be positive");
            return;
        }
        want = (std::min)(size_t(asked), WAB_EXPORT_CHUNK_BYTES);
    }
    want = (std::min)(want, size_t(size - std::uintmax_t(offset)));

    std::vector<char> buffer(want);
    if (want > 0) {
        boost::nowide::ifstream in(path_to_utf8(it->second), std::ios::binary);
        if (!in) {
            send_response(id, METHOD, WAB_INTERNAL, "the export could not be read");
            return;
        }
        in.seekg(offset);
        in.read(buffer.data(), std::streamsize(want));
        if (in.gcount() != std::streamsize(want)) {
            send_response(id, METHOD, WAB_INTERNAL, "the export was truncated while being read");
            return;
        }
    }

    std::string  encoded(boost::beast::detail::base64::encoded_size(want), char(0));
    const size_t written = boost::beast::detail::base64::encode(encoded.data(), buffer.data(), want);
    encoded.resize(written);

    const std::uintmax_t next = std::uintmax_t(offset) + want;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"offset", offset},
                   {"length", want},
                   {"nextOffset", next},
                   {"eof", next >= size},
                   {"base64", encoded}});
}

void WebAppBridge::Impl::handle_discard_export(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.discardExport";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    const std::string handle = json_string(params, "handle");
    const auto        it     = exports.find(handle);
    // Idempotent on purpose: a page cleaning up after a failure should not have to work out
    // whether the export survived.
    if (it != exports.end()) {
        boost::system::error_code ec;
        fs::remove(it->second, ec);
        exports.erase(it);
    }
    send_response(id, METHOD, WAB_OK, "success", {{"handle", handle}});
}

void WebAppBridge::Impl::handle_export_thumbnail(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.exportThumbnail";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }

    int plate_idx = plates.get_curr_plate_index();
    if (params.contains("plate")) {
        plate_idx = json_int(params, "plate", plate_idx);
        if (plate_idx < 0 || plate_idx >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }

    // Renders only the plates whose picture is stale, so the usual case costs nothing.
    plater->update_all_plate_thumbnails(false);

    PartPlate* plate = plates.get_plate(plate_idx);
    if (plate == nullptr || !plate->thumbnail_data.is_valid()) {
        // Rendering needs the 3D canvas, and the canvas is hidden while a web-application tab is
        // in front. Rather than hand back a blank or half-drawn image, say what is missing: the
        // Prepare tab has to have been shown at least once since the plate last changed.
        send_response(id, METHOD, WAB_NOT_FOUND,
                      "no picture of the plate is available yet -- open the Prepare tab once, then try again");
        return;
    }

    const ThumbnailData& thumb = plate->thumbnail_data;
    auto                 png   = GCodeThumbnails::compress_thumbnail(thumb, GCodeThumbnailsFormat::PNG);
    if (!png || png->data == nullptr || png->size == 0) {
        send_response(id, METHOD, WAB_INTERNAL, "the plate picture could not be encoded");
        return;
    }

    std::string  encoded(boost::beast::detail::base64::encoded_size(png->size), char(0));
    const size_t written = boost::beast::detail::base64::encode(encoded.data(), png->data, png->size);
    encoded.resize(written);

    send_response(id, METHOD, WAB_OK, "success",
                  {{"plate", plate_idx},
                   {"mime", "image/png"},
                   {"width", thumb.width},
                   {"height", thumb.height},
                   {"sizeBytes", png->size},
                   {"base64", encoded}});
}

void WebAppBridge::Impl::handle_slice(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.slice";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->only_gcode_mode()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "a G-code file is open; there is no model to slice (File > New Project first)");
        return;
    }
    if (params.contains("plate")) {
        const int plate = json_int(params, "plate", -1);
        if (plate < 0 || plate >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
        if (plate != plates.get_curr_plate_index())
            plater->select_plate(plate);
    }
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr || !plate->has_printable_instances()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate has no printable object");
        return;
    }

    // Plater::reslice() is the Slice button: it validates, may leave a notification instead of
    // starting, and reports nothing back. The outcome arrives as slicer.sliceStarted /
    // slicer.sliceFinished events; `accepted` says whether a slice is actually under way.
    plater->reslice();
    const bool started = plater->is_background_process_slicing();
    send_response(id, METHOD, WAB_OK, started ? "accepted" : "not started",
                  {{"accepted", started}, {"plate", plates.get_curr_plate_index()}, {"sliceResultValid", plate->is_slice_result_valid()}});
}

void WebAppBridge::Impl::handle_get_slice_result(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSliceResult";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    int            index  = plates.get_curr_plate_index();
    if (params.contains("plate")) {
        index = json_int(params, "plate", -1);
        if (index < 0 || index >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }
    PartPlate* plate = plates.get_plate(index);
    if (plate == nullptr || !plate->is_slice_result_valid()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "plate " + std::to_string(index) + " has no valid slice result");
        return;
    }
    json data     = WebAppSettings::slice_summary_json(plate);
    data["plate"] = index;
    send_response(id, METHOD, WAB_OK, "success", data);
}

// ---------------------------------------------------------------------------------------------
// Printer profiles (WEB_APP_BRIDGE.md section 4.4; policy, dialog and import in
// WebAppPresetShare.cpp)
//
// Export is the publish direction of a printer, under the same opt-in as exporting the plate: the
// slicer writes the edited printer preset -- flattened, print host and local paths removed -- into
// its cache and hands back an export handle that readExportChunk / discardExport serve unchanged.
// Install is the other direction under its own opt-in, and is never the page's decision alone:
// the file is downloaded on the same terms as openProjectFromUrl, sanitised again, and installed
// only if the user says so in a native dialog the page can neither skip nor pre-answer.
// ---------------------------------------------------------------------------------------------

void WebAppBridge::Impl::handle_export_printer_preset(const std::string& id, const json& /*params*/)
{
    static const char* METHOD = "slicer.exportPrinterPreset";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (exports.size() >= WAB_MAX_LIVE_EXPORTS) {
        send_response(id, METHOD, WAB_BUSY, "too many exports are still open; discard the ones you have finished with");
        return;
    }

    std::string stem = sanitise_filename(WebAppPresetShare::edited_printer_preset_name());
    if (stem.empty())
        stem = "printer";
    const std::string handle   = make_session_token().substr(0, 16);
    const fs::path    out_path = webapp_cache_dir() / path_from_utf8(stem + "-" + handle + ".json");

    const WebAppPresetShare::ExportResult result = WebAppPresetShare::export_edited_printer_preset(out_path);
    boost::system::error_code             ec;
    if (result.code != WAB_OK) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, result.code, result.message);
        return;
    }
    const std::uintmax_t size = fs::file_size(out_path, ec);
    if (ec || size == 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the export produced no file");
        return;
    }

    exports[handle] = out_path;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"filename", stem + ".json"},
                   {"sizeBytes", size},
                   {"chunkBytes", WAB_EXPORT_CHUNK_BYTES},
                   {"name", result.name},
                   {"dirty", result.dirty},
                   {"inherits", result.inherits}});
}

void WebAppBridge::Impl::handle_install_printer_preset(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.installPrinterPreset";
    static const char* EVENT  = "slicer.presetInstalled";
    if (!owner.allows_install()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to install printer profiles");
        return;
    }
    const std::string url = json_string(params, "url");
    if (url.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "url is required");
        return;
    }
    std::string message;
    if (const int code = check_download_url(url, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    std::string name = sanitise_filename(json_string(params, "filename"));
    if (name.empty())
        name = sanitise_filename(Http::get_filename_from_url(url));
    if (name.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "filename is required");
        return;
    }
    if (const std::string ext = lowercase_extension(path_from_utf8(name)); ext != "json") {
        send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "file type '" + ext + "' is not accepted: a printer profile is a .json file");
        return;
    }
    // A dialog already open, or a slice running: the install ends in a modal of its own.
    Tab* tab = nullptr;
    if (const int code = preflight(true, Preset::TYPE_PRINTER, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (install_in_progress.load()) {
        send_response(id, METHOD, WAB_BUSY, "a printer profile is already being installed");
        return;
    }
    if (!claim_download()) {
        send_response(id, METHOD, WAB_BUSY, "another download is already in progress");
        return;
    }
    install_in_progress.store(true);

    DownloadJob job;
    job.id           = id;
    job.url          = url;
    job.expected_sha = boost::algorithm::to_lower_copy(json_string(params, "sha256"));
    // Never the page's name on disk: a profile must not overwrite a file openProjectFromUrl fetched.
    job.destination = webapp_cache_dir() / path_from_utf8("printer-profile-" + make_session_token().substr(0, 16) + ".json");
    job.size_limit  = WAB_MAX_PRESET_BYTES;
    job.on_http_error = [id](Impl& self, const std::string& message, unsigned status) {
        self.install_in_progress.store(false);
        int         code  = (status == 404 || status == 410) ? WAB_NOT_FOUND : WAB_IO;
        std::string error = message;
        if (status == 0 && boost::algorithm::starts_with(message, "HTTP body data size exceeded")) {
            code  = WAB_TOO_LARGE;
            error = "the file is larger than the " + std::to_string(WAB_MAX_PRESET_BYTES / (1024 * 1024)) +
                    " MiB a printer profile may be";
        }
        self.send_event(id, EVENT, {{"ok", false}, {"error", error}, {"code", code}});
    };
    job.on_save_error = [id](Impl& self, const std::string& error) {
        self.install_in_progress.store(false);
        self.send_event(id, EVENT, {{"ok", false}, {"error", error}, {"code", WAB_IO}});
    };
    job.on_ready = [id](Impl& self, const fs::path& destination) {
        // GUI thread with the tab alive (run_download checked); the dialog below is modal, and the
        // shared_ptr the caller holds keeps this Impl alive through it.
        const wxString                   tab_title = WebAppPanel::configured_title(self.owner.slot());
        WebAppPresetShare::InstallResult outcome;
        try {
            outcome = WebAppPresetShare::install_printer_preset(destination, tab_title, webapp_cache_dir());
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "WebAppBridge: installing a printer profile failed: " << e.what();
            outcome       = WebAppPresetShare::InstallResult();
            outcome.code  = WAB_INTERNAL;
            outcome.error = std::string("the profile could not be installed: ") + e.what();
        }
        boost::system::error_code ec;
        fs::remove(destination, ec);
        self.install_in_progress.store(false);

        json data = {{"ok", outcome.ok}};
        if (outcome.ok) {
            data["name"]     = outcome.name;
            data["selected"] = outcome.selected;
        } else if (outcome.cancelled) {
            data["cancelled"] = true;
        } else {
            data["error"] = outcome.error;
            data["code"]  = outcome.code;
        }
        self.send_event(id, EVENT, data);
    };

    // Accepted, not finished: the outcome -- installed, declined, or why not -- arrives as a
    // slicer.presetInstalled event carrying this id.
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    run_download(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------------

WebAppBridge::WebAppBridge(WebAppPanel& owner) : p(std::make_shared<Impl>(owner)) {}

WebAppBridge::~WebAppBridge()
{
    // Runs on the GUI thread. Any in-flight Http callback may still hold Impl alive through its
    // weak pointer, but from here on it must not touch the panel.
    if (p) {
        // Exports live in the cache directory until the page discards them. A page that closed
        // mid-publish never will, so the tab going away is the backstop.
        p->discard_all_exports();
        p->detached.store(true);
    }
}

void WebAppBridge::on_tab_shown(bool /*shown*/) {}

void WebAppBridge::notify(const std::string& method, const json& data)
{
    if (p)
        p->notify(method, data);
}

void WebAppBridge::notify_capabilities()
{
    if (p)
        p->notify("slicer.capabilitiesChanged", {{"capabilities", p->capabilities()}});
}

void WebAppBridge::on_document_loaded(const wxString& /*url*/)
{
    // A fresh token per document: it cannot be replayed after a navigation or reload, and a
    // cross-origin frame that can reach window.wx cannot read the main frame's JS to steal it.
    if (!p->owner.origin_is_allowed()) {
        p->session_token.clear();
        return;
    }
    p->session_token = make_session_token();
    p->inject_bridge_script();
}

void WebAppBridge::on_script_message(wxWebViewEvent& evt)
{
    const wxString message = evt.GetString();
    if (message.empty())
        return;

    json root = json::parse(message.ToUTF8().data(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: ignoring unparseable script message";
        return;
    }

    const std::string id     = json_string(root, "id");
    std::string       method = json_string(root, "method");
    json params = root.contains("params") && root["params"].is_object() ? root["params"] : json::object();

    // Legacy aliases, matching the envelope PrinterWebViewHandler already accepts.
    if (method.empty()) {
        method = json_string(root, "command");
        if (params.empty() && root.contains("data") && root["data"].is_object())
            params = root["data"];
    }

    // Anything outside our namespace belongs to another consumer of the shared "wx" channel.
    if (method.rfind("slicer.", 0) != 0)
        return;

    if (!p->owner.origin_is_allowed()) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: rejecting '" << method << "' from untrusted origin '"
                                   << p->owner.current_origin().ToUTF8().data() << "'";
        p->send_response(id, method, WAB_FORBIDDEN_ORIGIN, "this origin may not use the slicer bridge");
        return;
    }

    if (p->session_token.empty() || json_string(root, "token") != p->session_token) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: rejecting '" << method << "' with a missing or stale token";
        p->send_response(id, method, WAB_BAD_TOKEN, "missing or stale session token");
        return;
    }

    if (method == "slicer.getInfo") {
        p->handle_get_info(id);
    } else if (method == "slicer.openProject") {
        p->handle_open_project(id, params);
    } else if (method == "slicer.openProjectFromUrl") {
        p->handle_open_project_from_url(id, params);
    } else if (method == "slicer.selectPreset") {
        p->handle_select_preset(id, params);
    } else if (method == "slicer.listPresets") {
        p->handle_list_presets(id, params);
    } else if (method == "slicer.getSettingsSchema") {
        p->handle_get_settings_schema(id, params);
    } else if (method == "slicer.getSettings") {
        p->handle_get_settings(id, params);
    } else if (method == "slicer.setSettings") {
        p->handle_set_settings(id, params);
    } else if (method == "slicer.getPlate") {
        p->handle_get_plate(id);
    } else if (method == "slicer.exportProject") {
        p->handle_export_project(id, params);
    } else if (method == "slicer.readExportChunk") {
        p->handle_read_export_chunk(id, params);
    } else if (method == "slicer.discardExport") {
        p->handle_discard_export(id, params);
    } else if (method == "slicer.exportThumbnail") {
        p->handle_export_thumbnail(id, params);
    } else if (method == "slicer.exportPrinterPreset") {
        p->handle_export_printer_preset(id, params);
    } else if (method == "slicer.installPrinterPreset") {
        p->handle_install_printer_preset(id, params);
    } else if (method == "slicer.slice") {
        p->handle_slice(id, params);
    } else if (method == "slicer.getSliceResult") {
        p->handle_get_slice_result(id, params);
    } else {
        p->send_response(id, method, WAB_UNKNOWN_METHOD, "unknown method");
    }
}

} // namespace GUI
} // namespace Slic3r
