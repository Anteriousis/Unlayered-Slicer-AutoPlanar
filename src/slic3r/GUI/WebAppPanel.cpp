#include "WebAppPanel.hpp"

#include "WebAppBridge.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>
#include <wx/uri.h>
#include <wx/utils.h>

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

namespace {

// Development overrides. Read from the environment, never written back to AppConfig, so a
// developer pointing the tab at a scratch server cannot accidentally persist that.
const char* const ENV_URL      = "ORCA_WEB_APP_URL";

// The URL the tab shipped with before it pointed at the real site. Per-key seeding below only
// fills values that are ABSENT, so an installation seeded by one of those builds would keep this
// dead address forever. Kept verbatim so the one-shot correction can match it exactly.
const char* const LEGACY_DEV_URL = "http://127.0.0.1:8765";
const char* const ENV_TITLE    = "ORCA_WEB_APP_TITLE";
const char* const ENV_DEVTOOLS = "ORCA_WEB_APP_DEVTOOLS";
// The agent slot's counterparts. Devtools stays shared: it is a property of the developer, not
// of a tab.
const char* const ENV_AGENT_URL   = "ORCA_AGENT_APP_URL";
const char* const ENV_AGENT_TITLE = "ORCA_AGENT_APP_TITLE";
// The Universe slot's counterparts, for pointing the tab at a local copy of the site.
// The one-shot visibility tag this edition applies (WebAppSlot::visibility_once). Shared by the
// two tabs that ship switched on, so both arrive in the same single pass over the configuration.
// Change it only to re-arm the one-shot; a customer install compares against what it has stored.
const char* const FORWARD_EDITION_TAG = "tabs-on-2026-09-20";

const char* const ENV_UNIVERSE_URL   = "ORCA_UNIVERSE_APP_URL";
const char* const ENV_UNIVERSE_TITLE = "ORCA_UNIVERSE_APP_TITLE";

wxString env_or_empty(const char* name)
{
    wxString value;
    if (wxGetEnv(name, &value))
        return value;
    return wxString();
}

// Percent-encode a string for use as a query-parameter value. wxURI has no public encoder, and
// the fallback page carries a URL and a free-text title in its query string.
wxString percent_encode(const wxString& value)
{
    static const char* hex = "0123456789ABCDEF";
    const auto  utf8 = value.ToUTF8();
    wxString    out;
    for (const char* p = utf8.data(); p != nullptr && *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wxChar>(c);
        } else {
            out += '%';
            out += static_cast<wxChar>(hex[(c >> 4) & 0x0F]);
            out += static_cast<wxChar>(hex[c & 0x0F]);
        }
    }
    return out;
}

// Seed a slot's shipped defaults exactly once. There are two sources, and on a customer machine
// only the first of them exists: the slot's own default_* fields, and then resources/web/webapp/
// as an overlay. The files used to be the whole story -- that is what kept application-specific
// strings out of the source tree (WEB_APP_BRIDGE.md) -- but they are excluded from the installer,
// so a shipped build finds nothing to read and the slot has to carry its own address. The file is
// still read where it exists, which is how a development tree changes an address without a
// rebuild; ORCA_*_APP_URL does the same and wins over both.
void ensure_seeded(const WebAppSlot& slot)
{
    // Once per slot per process, keyed by id.
    static std::set<std::string> s_checked;
    if (!s_checked.insert(slot.id).second)
        return;

    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;

    // Whether this is the very first time this installation has seeded this slot. It gates the
    // booleans only -- see below.
    const bool first_seed = !cfg->get_bool(slot.key_seeded);

    // The one-shot change of visibility, before the defaults below, so that an address it clears
    // is settled before anything asks whether one is set.
    if (slot.visibility_once != nullptr && cfg->get(slot.key_disable_once) != slot.visibility_once) {
        if (cfg->get_bool(slot.key_enabled) != slot.visibility_once_enable) {
            BOOST_LOG_TRIVIAL(info) << "WebAppPanel[" << slot.id << "]: one-shot "
                                    << (slot.visibility_once_enable ? "enable" : "disable");
            cfg->set_bool(slot.key_enabled, slot.visibility_once_enable);
        }
        // Taking a tab away clears its address as well: is_enabled() wants the flag AND a
        // non-empty url, so an emptied address means the tab cannot be restored by flipping one
        // boolean in the configuration file. Adding one leaves the address to the defaults below,
        // which fill it precisely because an earlier edition emptied it.
        if (!slot.visibility_once_enable)
            cfg->set(slot.key_url, "");
        cfg->set(slot.key_disable_once, slot.visibility_once);
    }

    // The defaults this build carries, applied per key rather than by one all-or-nothing flag: a
    // key added in a later release has to be able to reach an installation seeded before it
    // existed.
    //
    // The address and the label are authoritative, not a fallback for when the key is missing.
    // These tabs have no Preferences UI, so a value already in the configuration cannot be a
    // choice the user made -- it can only be what an earlier build seeded. Deferring to it would
    // mean a renamed tab or a moved address reached fresh installations and no others, which is
    // the worst of both. A development machine points a tab elsewhere with ORCA_*_APP_URL, which
    // wins over this and is never written back.
    if (slot.default_url != nullptr && cfg->get(slot.key_url) != slot.default_url)
        cfg->set(slot.key_url, slot.default_url);
    if (slot.default_title != nullptr && cfg->get(slot.key_title) != slot.default_title)
        cfg->set(slot.key_title, slot.default_title);
    if (first_seed)
        cfg->set_bool(slot.key_allow_settings, slot.default_allow_settings);
    if (!cfg->has(slot.key_allow_export))
        cfg->set_bool(slot.key_allow_export, slot.default_allow_export);

    // Recorded here rather than after the overlay below, because the defaults above are a seed in
    // their own right and a shipped build never reaches the overlay at all. Without this the
    // first-seed booleans would be re-applied on every launch, overwriting the user's choice.
    cfg->set_bool(slot.key_seeded, true);
    cfg->save();

    const boost::filesystem::path seed_path =
        boost::filesystem::path(resources_dir()) / "web" / "webapp" / slot.seed_file;

    boost::system::error_code ec;
    if (!boost::filesystem::exists(seed_path, ec) || ec)
        return;

    try {
        boost::nowide::ifstream stream(seed_path.string());
        json                    seed;
        stream >> seed;

        // Seeding is decided PER KEY, on whether the config already carries a value, rather than
        // by one all-or-nothing flag. The flag alone was wrong: it meant a key added to the seed
        // file in a later release could never reach an installation that had already been seeded
        // once -- which is exactly what happened when extra_origins was introduced. AppConfig::has
        // treats an empty string as absent, so a key present-but-blank is still fillable, and a
        // value the user actually set is never touched.
        if (seed.contains("url") && seed["url"].is_string() && !cfg->has(slot.key_url))
            cfg->set(slot.key_url, seed["url"].get<std::string>());
        if (seed.contains("title") && seed["title"].is_string() && !cfg->has(slot.key_title))
            cfg->set(slot.key_title, seed["title"].get<std::string>());
        // A slot whose site serves downloads from a different host than the page needs that host
        // named or openProjectFromUrl refuses every fetch. Seeded like the strings above, so an
        // installation that already exists still picks it up, and a value the user set survives.
        if (seed.contains("download_hosts") && seed["download_hosts"].is_string() &&
            !cfg->has(slot.key_download_hosts))
            cfg->set(slot.key_download_hosts, seed["download_hosts"].get<std::string>());
        // The booleans are the exception: a bool has no "unset" state, so false is indistinguishable
        // from "the user turned it off". Only ever applied on the first seed.
        if (first_seed && seed.contains("enabled") && seed["enabled"].is_boolean())
            cfg->set_bool(slot.key_enabled, seed["enabled"].get<bool>());
        if (first_seed && seed.contains("allow_settings") && seed["allow_settings"].is_boolean())
            cfg->set_bool(slot.key_allow_settings, seed["allow_settings"].get<bool>());
        // Seeded on absence, not on first_seed, and deliberately unlike `enabled` and
        // `allow_settings` above. Those two are first-seed-only because a stored false cannot be
        // told apart from the user having switched them off. This key is newer than the installs
        // it has to reach: an installation seeded before it existed has no value for it at all,
        // and first-seed-only would mean the permission could never arrive there. AppConfig gives
        // it no default for the same reason, so "absent" survives long enough to be detected, and
        // a false the user actually chose is non-empty and is left alone.
        if (seed.contains("allow_export") && seed["allow_export"].is_boolean() &&
            !cfg->has(slot.key_allow_export))
            cfg->set_bool(slot.key_allow_export, seed["allow_export"].get<bool>());
        if (seed.contains("extra_origins") && seed["extra_origins"].is_array() &&
            !cfg->has(slot.key_extra_origins)) {
            std::string joined;
            for (const auto& o : seed["extra_origins"])
                if (o.is_string()) {
                    if (!joined.empty()) joined += ";";
                    joined += o.get<std::string>();
                }
            cfg->set(slot.key_extra_origins, joined);
        }

        // One-shot correction for configs seeded by a build whose default was the local dev
        // server. Only a slot with that history carries legacy_url, and only this exact value is
        // rewritten, so a URL the user actually chose is never touched; a developer wanting the
        // local server should use the slot's env_url override, which wins over AppConfig and is
        // never written back.
        if (slot.legacy_url != nullptr && seed.contains("url") && seed["url"].is_string() &&
            cfg->get(slot.key_url) == slot.legacy_url) {
            const std::string seeded_url = seed["url"].get<std::string>();
            if (seeded_url != slot.legacy_url) {
                BOOST_LOG_TRIVIAL(info) << "WebAppPanel[" << slot.id << "]: replacing the old development URL with " << seeded_url;
                cfg->set(slot.key_url, seeded_url);
            }
        }

        cfg->set_bool(slot.key_seeded, true);
        cfg->save();

        BOOST_LOG_TRIVIAL(info) << "WebAppPanel[" << slot.id << "]: seeded defaults from " << seed_path.string();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppPanel: could not read " << seed_path.string() << ": " << e.what();
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------------------------

const WebAppSlot& WebAppSlot::generic()
{
    static const WebAppSlot s_slot = [] {
        WebAppSlot s{};
        s.id                 = "web_app";
        s.key_enabled        = SETTING_WEB_APP_ENABLED;
        s.key_url            = SETTING_WEB_APP_URL;
        s.key_title          = SETTING_WEB_APP_TITLE;
        s.key_allow_local    = SETTING_WEB_APP_ALLOW_LOCAL;
        s.key_path_roots     = SETTING_WEB_APP_PATH_ROOTS;
        s.key_download_hosts = SETTING_WEB_APP_DOWNLOAD_HOSTS;
        s.key_extra_origins  = SETTING_WEB_APP_EXTRA_ORIGINS;
        s.key_seeded         = SETTING_WEB_APP_SEEDED;
        s.key_allow_settings = SETTING_WEB_APP_ALLOW_SETTINGS;
        s.key_allow_export   = SETTING_WEB_APP_ALLOW_EXPORT;
        s.key_disable_once   = SETTING_WEB_APP_DISABLE_ONCE;
        s.env_url            = ENV_URL;
        s.env_title          = ENV_TITLE;
        s.seed_file          = "default.json";
        // Still withdrawn. This tab hosts whatever a developer points it at, which is not a
        // feature of the shipped product; the tag is the one the customer edition applied, so an
        // installation that has already had it taken away is left alone.
        s.visibility_once        = "customer-edition-2026-09-19";
        s.visibility_once_enable = false;
        s.default_url            = nullptr; // ships with no address, so the tab cannot appear
        s.default_title          = nullptr;
        s.default_allow_settings = false;
        s.default_allow_export   = false;
        s.download_hosts     = nullptr;
        s.legacy_url         = LEGACY_DEV_URL;
        s.live_theme         = false; // reloads on a theme toggle, as it always has
        return s;
    }();
    return s_slot;
}

const WebAppSlot& WebAppSlot::agent()
{
    static const WebAppSlot s_slot = [] {
        WebAppSlot s{};
        s.id                 = "agent_app";
        s.key_enabled        = SETTING_AGENT_APP_ENABLED;
        s.key_url            = SETTING_AGENT_APP_URL;
        s.key_title          = SETTING_AGENT_APP_TITLE;
        s.key_allow_local    = SETTING_AGENT_APP_ALLOW_LOCAL;
        s.key_path_roots     = SETTING_AGENT_APP_PATH_ROOTS;
        s.key_download_hosts = SETTING_AGENT_APP_DOWNLOAD_HOSTS;
        s.key_extra_origins  = SETTING_AGENT_APP_EXTRA_ORIGINS;
        s.key_seeded         = SETTING_AGENT_APP_SEEDED;
        s.key_allow_settings = SETTING_AGENT_APP_ALLOW_SETTINGS;
        s.key_allow_export   = SETTING_AGENT_APP_ALLOW_EXPORT;
        s.key_disable_once   = SETTING_AGENT_APP_DISABLE_ONCE;
        s.env_url            = ENV_AGENT_URL;
        s.env_title          = ENV_AGENT_TITLE;
        s.seed_file          = "agent.json";
        // A shipped tab again. The tag differs from the customer edition's, so an installation
        // that release took the tab away from gets it back exactly once; a fresh installation has
        // no tag at all and so also matches. Either way the address below is filled, because the
        // withdrawal cleared it.
        s.visibility_once        = FORWARD_EDITION_TAG;
        s.visibility_once_enable = true;
        s.default_url            = "https://unlayered3d.com/agent?host=unlayered-slicer";
        s.default_title          = "Agent";
        // The agent's whole purpose is to change print settings on the user's behalf, so the
        // settings family of the bridge is on for this tab and off for the other two.
        s.default_allow_settings = true;
        s.default_allow_export   = false;
        s.download_hosts     = nullptr;
        s.legacy_url         = nullptr; // no build ever seeded this slot with a dead address
        s.live_theme         = true;    // the page holds a conversation; re-theme it in place
        return s;
    }();
    return s_slot;
}

const WebAppSlot& WebAppSlot::universe()
{
    static const WebAppSlot s_slot = [] {
        WebAppSlot s{};
        s.id                 = "universe_app";
        s.key_enabled        = SETTING_UNIVERSE_APP_ENABLED;
        s.key_url            = SETTING_UNIVERSE_APP_URL;
        s.key_title          = SETTING_UNIVERSE_APP_TITLE;
        s.key_allow_local    = SETTING_UNIVERSE_APP_ALLOW_LOCAL;
        s.key_path_roots     = SETTING_UNIVERSE_APP_PATH_ROOTS;
        s.key_download_hosts = SETTING_UNIVERSE_APP_DOWNLOAD_HOSTS;
        s.key_extra_origins  = SETTING_UNIVERSE_APP_EXTRA_ORIGINS;
        s.key_seeded         = SETTING_UNIVERSE_APP_SEEDED;
        s.key_allow_settings = SETTING_UNIVERSE_APP_ALLOW_SETTINGS;
        s.key_allow_export   = SETTING_UNIVERSE_APP_ALLOW_EXPORT;
        s.key_disable_once   = SETTING_UNIVERSE_APP_DISABLE_ONCE;
        s.env_url            = ENV_UNIVERSE_URL;
        s.env_title          = ENV_UNIVERSE_TITLE;
        s.seed_file          = "universe.json";
        // Shares the agent's tag: the two tabs arrive together and this slot has no separate
        // history to respect -- it has never shipped switched on.
        s.visibility_once        = FORWARD_EDITION_TAG;
        s.visibility_once_enable = true;
        s.default_url            = "https://unlayered3d.com/universe?host=unlayered-slicer";
        s.default_title          = "Universe";
        // Universe browses and imports models; it has no business reading or writing print
        // settings. Export is the publish direction -- the page asks for what is on the plate and
        // reads the bytes back -- and is a different thing to consent to, hence a separate gate.
        s.default_allow_settings = false;
        s.default_allow_export   = true;
        // Universe answers a download with a redirect to blob storage, which is a DIFFERENT
        // host from the page and whose sub-domain belongs to the deployment -- so no exact
        // host can be baked in, and a leading dot matches the family. Without this every
        // "open in the slicer" is refused with 1003, and there is no Preferences field for
        // it any more.
        s.download_hosts     = ".vercel-storage.com";
        s.legacy_url         = nullptr; // no build ever seeded this slot with a dead address
        // Browsing state is cheap to rebuild and the site themes itself from a cookie on load,
        // so a reload on a theme toggle is the honest behaviour here -- unlike the agent tab,
        // whose page holds a conversation that a reload would throw away.
        s.live_theme         = false;
        return s;
    }();
    return s_slot;
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

wxString WebAppPanel::configured_url(const WebAppSlot& slot)
{
    const wxString from_env = env_or_empty(slot.env_url);
    if (!from_env.empty())
        return from_env;

    ensure_seeded(slot);
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return wxString();
    return wxString::FromUTF8(cfg->get(slot.key_url).c_str());
}

wxString WebAppPanel::configured_title(const WebAppSlot& slot)
{
    const wxString from_env = env_or_empty(slot.env_title);
    if (!from_env.empty())
        return from_env;

    ensure_seeded(slot);
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        const wxString title = wxString::FromUTF8(cfg->get(slot.key_title).c_str());
        if (!title.empty())
            return title;
    }
    return _L("Web App");
}

bool WebAppPanel::is_enabled(const WebAppSlot& slot)
{
    // Not in a release build, whatever the config or the ORCA_*_APP_URL environment says:
    // - the generic slot is a developer harness, not part of the product;
    // - the Agent tab is not part of this release.
    // The dev edition keeps both.
    if (std::string(slot.id) == WebAppSlot::generic().id || std::string(slot.id) == WebAppSlot::agent().id)
        return false;
    // An environment override implies the developer wants the tab, whatever the config says.
    if (!env_or_empty(slot.env_url).empty())
        return true;

    ensure_seeded(slot);
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return false;
    // A tab pointing at nothing is worse than no tab at all.
    return cfg->get_bool(slot.key_enabled) && cfg->has(slot.key_url);
}

wxString WebAppPanel::origin_of(const wxString& url)
{
    if (url.empty())
        return wxString();

    const wxURI uri(url);
    if (!uri.HasScheme() || !uri.HasServer())
        return wxString();

    const wxString scheme = uri.GetScheme().Lower();
    const wxString host   = uri.GetServer().Lower();
    if (host.empty())
        return wxString();

    wxString origin = scheme + "://" + host;

    if (uri.HasPort()) {
        const wxString port = uri.GetPort();
        const bool     is_default = (scheme == "http" && port == "80") || (scheme == "https" && port == "443");
        if (!port.empty() && !is_default)
            origin += ":" + port;
    }
    return origin;
}

bool WebAppPanel::is_loopback_origin(const wxString& origin)
{
    if (origin.empty())
        return false;

    const wxURI uri(origin);
    if (!uri.HasServer())
        return false;

    wxString host = uri.GetServer().Lower();
    // wxURI keeps the brackets on an IPv6 literal.
    host.Replace("[", "");
    host.Replace("]", "");

    return host == "localhost" || host == "::1" || host.StartsWith("127.");
}

bool WebAppPanel::is_secure_context(const wxString& url)
{
    const wxString origin = origin_of(url);
    if (origin.empty())
        return false;
    if (origin.StartsWith("https://"))
        return true;
    // Plain http is permitted only to loopback, which browsers and the W3C treat as a
    // trustworthy origin. Anything else would put the bridge on the far side of the network.
    return origin.StartsWith("http://") && is_loopback_origin(origin);
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

std::vector<WebAppPanel*> WebAppPanel::s_live;

WebAppPanel::WebAppPanel(wxWindow* parent, const WebAppSlot& slot)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_slot(slot)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(topsizer);

#if wxUSE_WEBVIEW_EDGE
    if (!WebView::CheckWebViewRuntime()) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppPanel: WebView2 runtime is not installed";
        show_runtime_missing_panel();
        return;
    }
#endif

    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "WebAppPanel: could not create the web view";
        show_runtime_missing_panel();
        return;
    }

#ifdef __linux__
    // NB: the WebKitGTK cookie store is per-process and PrinterWebView already points it at
    // <data_dir>/cache/cookies.db. Setting it a second time with a different path would be a
    // silent no-op at best, so this panel deliberately inherits whatever that set up.
#endif

    build_offsite_bar();
    topsizer->Add(m_offsite_bar, 0, wxEXPAND);

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &WebAppPanel::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &WebAppPanel::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &WebAppPanel::OnNavigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &WebAppPanel::OnNewWindow, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebAppPanel::OnScriptMessage, this);
    // Theme toggle: WebView::RecreateAll asks each web view whether it re-themes in place before
    // falling back to a reload. See OnWebViewRecreated.
    m_browser->Bind(EVT_WEBVIEW_RECREATED, &WebAppPanel::OnWebViewRecreated, this);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    m_bridge = std::make_unique<WebAppBridge>(*this);
    s_live.push_back(this);

    update_mode();

    Bind(wxEVT_CLOSE_WINDOW, &WebAppPanel::OnClose, this);
}

WebAppPanel::~WebAppPanel()
{
    SetEvtHandlerEnabled(false);
    // Out of the notify_all set before the bridge goes, so no event can reach a half-destroyed
    // panel.
    s_live.erase(std::remove(s_live.begin(), s_live.end(), this), s_live.end());
    m_bridge.reset();

    if (m_browser != nullptr) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void WebAppPanel::show_runtime_missing_panel()
{
    wxStaticText* label = new wxStaticText(
        this, wxID_ANY,
        _L("This tab needs the Microsoft Edge WebView2 runtime, which is not installed."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    label->Wrap(FromDIP(420));
    GetSizer()->AddStretchSpacer();
    GetSizer()->Add(label, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(20));
    GetSizer()->AddStretchSpacer();
}

void WebAppPanel::OnClose(wxCloseEvent& evt) { this->Hide(); }

// ---------------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------------

void WebAppPanel::load_home()
{
    if (m_browser == nullptr)
        return;

    m_home_url = configured_url(m_slot);

    if (m_home_url.empty()) {
        show_unavailable_page(wxString(), "unconfigured");
        return;
    }
    if (origin_of(m_home_url).empty()) {
        show_unavailable_page(m_home_url, "malformed");
        return;
    }
    if (!is_secure_context(m_home_url)) {
        // Refuse outright rather than loading and hoping. There is deliberately no override.
        BOOST_LOG_TRIVIAL(warning) << "WebAppPanel: refusing to load insecure URL " << m_home_url.ToUTF8().data();
        show_unavailable_page(m_home_url, "insecure");
        return;
    }

    load_url(m_home_url);
}

void WebAppPanel::load_url(const wxString& url)
{
    if (m_browser == nullptr || url.empty())
        return;

    m_showing_fallback = false;

    if (IsShown()) {
        m_browser->LoadURL(url);
    } else {
        // Deferred, as PrinterWebView does: nothing is fetched until the user opens the tab.
        m_url_deferred = url;
    }
}

bool WebAppPanel::Show(bool show)
{
    if (show && m_browser != nullptr && !m_url_deferred.empty()) {
        // m_url_deferred is cleared in OnLoaded, not here, so a failed load retries next time
        // the tab is shown.
        m_browser->LoadURL(m_url_deferred);
    }
    if (m_bridge)
        m_bridge->on_tab_shown(show);
    return wxPanel::Show(show);
}

void WebAppPanel::reload()
{
    if (m_browser != nullptr)
        m_browser->Reload();
}

void WebAppPanel::update_mode()
{
    if (m_browser == nullptr)
        return;

    bool enable = wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("developer_mode");
    wxString value;
    if (wxGetEnv(ENV_DEVTOOLS, &value) && value == "1")
        enable = true;

    m_browser->EnableAccessToDevTools(enable);
}

std::vector<wxString> WebAppPanel::extra_origins(const WebAppSlot& slot)
{
    std::vector<wxString> out;
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return out;

    const wxString configured = wxString::FromUTF8(cfg->get(slot.key_extra_origins).c_str());
    wxStringTokenizer tok(configured, ";,", wxTOKEN_STRTOK);
    while (tok.HasMoreTokens()) {
        wxString entry = tok.GetNextToken();
        entry.Trim(true).Trim(false);
        const wxString origin = origin_of(entry);
        // https only. These are third-party sites reached from inside the application, often to
        // buy something -- plain http would be indefensible.
        if (!origin.empty() && origin.StartsWith("https://"))
            out.push_back(origin);
    }
    return out;
}

bool WebAppPanel::is_navigable(const wxString& url) const
{
    if (url.empty() || url == "about:blank")
        return true;

    // Our own fallback page.
    if (url.StartsWith("file://"))
        return url.Contains("/web/webapp/");

    const wxString origin = origin_of(url);
    if (origin.empty())
        return false;
    if (origin == origin_of(m_home_url))
        return true;

    // Allow-listed third-party origins. Navigable, deliberately NOT trusted: the bridge shim is
    // injected only when origin_is_allowed() holds, which is an exact match on the configured
    // origin, so nothing reached here can drive the slicer.
    for (const wxString& allowed : extra_origins(m_slot))
        if (origin == allowed)
            return true;

    return false;
}

void WebAppPanel::build_offsite_bar()
{
    m_offsite_bar = new wxPanel(this, wxID_ANY);
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);

    wxButton* back = new wxButton(m_offsite_bar, wxID_ANY, _L("Back"), wxDefaultPosition, wxDefaultSize,
                                  wxBU_EXACTFIT);
    back->SetToolTip(_L("Return to the web application"));
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_home(); });

    // The tab has no address bar, so this label is the only way the user can see whose site they
    // are on. That matters most at exactly the moment it is least visible: a checkout page.
    m_offsite_label = new wxStaticText(m_offsite_bar, wxID_ANY, wxEmptyString);

    wxButton* external = new wxButton(m_offsite_bar, wxID_ANY, _L("Open in browser"), wxDefaultPosition,
                                      wxDefaultSize, wxBU_EXACTFIT);
    external->SetToolTip(_L("Open this page in your normal web browser, where your saved logins and "
                           "payment details are available"));
    external->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser != nullptr) {
            const wxString url = m_browser->GetCurrentURL();
            if (url.StartsWith("https://") || url.StartsWith("http://"))
                wxLaunchDefaultBrowser(url);
        }
    });

    row->Add(back, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    row->Add(m_offsite_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    row->Add(external, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    m_offsite_bar->SetSizer(row);
    m_offsite_bar->Hide();
}

void WebAppPanel::update_offsite_bar()
{
    if (m_offsite_bar == nullptr)
        return;

    const wxString home = origin_of(m_home_url);
    const bool offsite = !m_current_origin.empty() && !home.empty() && m_current_origin != home;

    if (offsite && m_offsite_label != nullptr) {
        wxString host = m_current_origin;
        host.Replace("https://", "");
        host.Replace("http://", "");
        m_offsite_label->SetLabel(wxString::Format(_L("You are on %s — outside %s"), host,
                                                   wxString(SLIC3R_APP_NAME)));
    }
    if (m_offsite_bar->IsShown() != offsite) {
        m_offsite_bar->Show(offsite);
        Layout();
    }
}

void WebAppPanel::launch_external(const wxString& url)
{
    if (!url.StartsWith("http://") && !url.StartsWith("https://"))
        return;

    // One user action must open at most one browser tab. A window.open() can reach us as BOTH a
    // new-window request and a top-level navigation, and each handler used to launch on its own,
    // so a single click on "open this in my browser" opened two. Rather than guess which event
    // the backend will send -- it differs between Edge and WebKit, and between a target=_blank
    // link and a scripted open -- both go through here and a repeat of the same URL inside a
    // short window is dropped.
    const wxLongLong now = wxGetUTCTimeMillis();
    if (url == m_last_external_url && (now - m_last_external_ms) < 2000) {
        BOOST_LOG_TRIVIAL(debug) << "WebAppPanel: not opening " << url.ToUTF8().data() << " twice";
        return;
    }
    m_last_external_url = url;
    m_last_external_ms  = now;

    wxLaunchDefaultBrowser(url);
}

void WebAppPanel::OnNavigating(wxWebViewEvent& evt)
{
    // wxEVT_WEBVIEW_NAVIGATING fires for EVERY frame, not just the top one: the Edge backend hooks
    // both NavigationStarting and FrameNavigationStarting and flags the main frame with
    // GetInt() == 1 (see wxWebViewEdgeImpl::HandleNavigationStarting).
    //
    // Enforcing the origin allow-list on sub-frames was wrong twice over. It vetoed every
    // third-party iframe an ordinary commerce page carries -- analytics, chat widget, payment,
    // captcha -- breaking the page, AND it called wxLaunchDefaultBrowser for each one, so a single
    // click spawned a pile of browser tabs.
    //
    // Sub-frames are ordinary web content and are not a way into the slicer: the bridge shim and
    // its session token are injected into the main frame's document only, so an iframe has no
    // window.__slicerBridge to call. Let them load.
    if (evt.GetInt() != 1)
        return;

    const wxString url = evt.GetURL();

    // A URL just handed to the system browser must not also load in this tab. A target="_blank"
    // link arrives as a new-window request, which we answer by opening the real browser and
    // vetoing; some backends then fall back to navigating the current frame with the same URL. On
    // a same-origin link -- a terms or privacy page, say -- that fallback is "navigable", so the
    // tab would walk away from whatever the user was filling in, losing a part-completed form and
    // any box they had ticked. Opening it in the browser was the whole answer; stay put.
    if (url == m_last_external_url && (wxGetUTCTimeMillis() - m_last_external_ms) < 2000) {
        BOOST_LOG_TRIVIAL(debug) << "WebAppPanel: staying put; " << url.ToUTF8().data()
                                 << " went to the browser";
        evt.Veto();
        return;
    }

    if (is_navigable(url))
        return;

    BOOST_LOG_TRIVIAL(info) << "WebAppPanel: vetoing top-level navigation to " << url.ToUTF8().data();
    evt.Veto();

    // Only reached for a top-level navigation the user actually triggered, so handing it to the
    // real browser is the expected outcome -- notably at checkout, which typically redirects to a
    // payment origin we would not allow-list anyway, and which the user wants in the browser where
    // their cards and logins live.
    launch_external(url);
}

void WebAppPanel::OnNewWindow(wxWebViewEvent& evt)
{
    // A popup the page asked for. The slicer has no popup window to give it, so the real browser
    // gets it instead -- which is also how a page deliberately escapes the tab, for a sign-in the
    // embedded view cannot complete.
    launch_external(evt.GetURL());
    evt.Veto();
}

void WebAppPanel::OnLoaded(wxWebViewEvent& evt)
{
    if (evt.GetURL().IsEmpty())
        return;

    m_url_deferred.clear();
    m_current_origin = origin_of(evt.GetURL());
    update_offsite_bar();

    if (m_bridge)
        m_bridge->on_document_loaded(evt.GetURL());
}

void WebAppPanel::OnError(wxWebViewEvent& evt)
{
    wxString reason = "other";
    switch (evt.GetInt()) {
    case wxWEBVIEW_NAV_ERR_CONNECTION:    reason = "connection";  break;
    case wxWEBVIEW_NAV_ERR_CERTIFICATE:   reason = "certificate"; break;
    case wxWEBVIEW_NAV_ERR_AUTH:          reason = "auth";        break;
    case wxWEBVIEW_NAV_ERR_SECURITY:      reason = "security";    break;
    case wxWEBVIEW_NAV_ERR_NOT_FOUND:     reason = "notfound";    break;
    case wxWEBVIEW_NAV_ERR_REQUEST:       reason = "request";     break;
    case wxWEBVIEW_NAV_ERR_USER_CANCELLED: return; // not a failure worth a whole page
    case wxWEBVIEW_NAV_ERR_OTHER:
    default:                              reason = "other";       break;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("WebAppPanel: error loading %1%: %2% (%3%)") % evt.GetURL().ToUTF8().data() %
                                   reason.ToUTF8().data() % evt.GetString().ToUTF8().data();

    // The fallback page itself failing must not recurse.
    if (m_showing_fallback)
        return;

    show_unavailable_page(m_home_url, reason);
}

void WebAppPanel::show_unavailable_page(const wxString& failed_url, const wxString& reason)
{
    if (m_browser == nullptr)
        return;

    m_showing_fallback = true;
    m_current_origin.clear();
    update_offsite_bar();

    const wxString page = wxString::Format("file://%s/web/webapp/unavailable.html",
                                           wxString::FromUTF8(resources_dir().c_str()));
    const wxString url = page + "?reason=" + percent_encode(reason) + "&url=" + percent_encode(failed_url) +
                         "&title=" + percent_encode(configured_title(m_slot));

    // Deliberately m_browser->LoadURL and not WebView::LoadUrl: the latter round-trips through
    // wxURI::BuildURI(), which would re-encode the query values we just encoded.
    m_browser->LoadURL(url);
}

void WebAppPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    if (m_bridge)
        m_bridge->on_script_message(evt);
}

void WebAppPanel::OnWebViewRecreated(wxCommandEvent& evt)
{
    // WebView::RecreateAll fires this on a theme toggle and reloads the page unless the event is
    // handled. The generic slot keeps that reload: its page re-themes itself on load, as every
    // other web view here does. A live_theme slot instead tells the page through the bridge and
    // does NOT Skip(), so the reload is skipped and the page keeps its state -- the precedent is
    // WebViewHostDialog::on_webview_recreated. The fallback page has no bridge session to tell,
    // so it reloads as before.
    if (!m_slot.live_theme || m_showing_fallback || !m_bridge) {
        evt.Skip();
        return;
    }
    m_bridge->notify("slicer.themeChanged", json{{"darkMode", GUI_App::dark_mode()}});
}

bool WebAppPanel::allows_settings() const
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg != nullptr && cfg->get_bool(m_slot.key_allow_settings);
}

bool WebAppPanel::allows_export() const
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg != nullptr && cfg->get_bool(m_slot.key_allow_export);
}

void WebAppPanel::notify_all(const std::string& method, const json& data)
{
    for (WebAppPanel* panel : s_live)
        if (panel->bridge() != nullptr)
            panel->bridge()->notify(method, data);
}

bool WebAppPanel::origin_is_allowed() const
{
    // Exact string equality on the normalised origin. Never StartsWith or Contains -- that is
    // how "https://evil.example/?x=http://127.0.0.1:8765" gets treated as trusted.
    const wxString expected = origin_of(m_home_url);
    return !expected.empty() && !m_current_origin.empty() && m_current_origin == expected;
}

bool WebAppPanel::origin_is_loopback() const
{
    return is_loopback_origin(m_current_origin);
}

} // namespace GUI
} // namespace Slic3r
