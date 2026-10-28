#include "PrivacySetupDialog.hpp"

#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r { namespace GUI {

namespace {

std::string reply_js(const json &message)
{
    return "HandleStudio(" + message.dump(-1, ' ', false, json::error_handler_t::replace) + ")";
}

} // namespace

PrivacyFlow::PrivacyFlow(bool privacy_only, std::string choice_source)
    : m_privacy_only(privacy_only)
    , m_choice_source(std::move(choice_source))
    , m_stats([](const json &body) { PrivacyChoiceStats::submit(body); })
{
    // Nothing is selected for the person: they name the place they live themselves.
}

PrivacyFlow::~PrivacyFlow() { m_stats.finish(); }

std::string PrivacyFlow::guide_region() const
{
    const PrivacyConsent::Place *where = PrivacyConsent::place(m_place);
    return where ? where->guide_region : std::string();
}

PrivacyChoiceStats::Event PrivacyFlow::stats_event(const char *type) const
{
    const PrivacyConsent::Place *where = PrivacyConsent::place(m_place);
    const std::string region  = where ? where->region : "";
    const std::string country = where ? where->country : "";
    const PrivacyConsent::RegionPolicy policy = PrivacyConsent::policy_for(region, country, PrivacyConsent::legal_approvals());
    PrivacyChoiceStats::Event e;
    e.event_type     = type;
    e.region         = region;
    e.country        = country;
    e.page_version   = policy.page_version;
    e.notice_version = PrivacyConsent::NOTICE_VERSION;
    e.choice_source  = m_choice_source;
    return e;
}

bool PrivacyFlow::handle(const json &j, std::string &reply)
{
    reply.clear();
    const std::string cmd = j.value("command", std::string());

    if (cmd == "privacy_request_state") {
        const PrivacyConsent::Place *where = PrivacyConsent::place(m_place);
        const PrivacyConsent::RegionPolicy policy = PrivacyConsent::policy_for(where ? where->region : "", where ? where->country : "",
                                                                               PrivacyConsent::legal_approvals());
        json list = json::array();
        for (const PrivacyConsent::Place &p : PrivacyConsent::places())
            list.push_back({{"id", p.id}, {"label", p.label}});
        reply = reply_js({{"command", "privacy_state"},
                          {"privacy_only", m_privacy_only},
                          {"place", m_place},
                          {"places", list},
                          {"preselect_share", policy.preselect_share},
                          {"organization_off", PrivacyConsent::organization_opt_out()},
                          {"page_version", policy.page_version},
                          {"notice_version", PrivacyConsent::NOTICE_VERSION}});
        return true;
    }
    if (cmd == "privacy_save_place") {
        const std::string id = j.value("place", std::string());
        const bool        ok = PrivacyConsent::place(id) != nullptr;
        if (ok)
            m_place = id;
        reply = reply_js({{"command", "privacy_place_saved"}, {"ok", ok}});
        return true;
    }
    if (cmd == "privacy_page_shown") {
        m_stats.page_shown(stats_event("privacy_page_shown"));
        return true;
    }
    if (cmd == "privacy_submit") {
        const std::string choice = j.value("choice", std::string());
        const PrivacyConsent::Place *where = PrivacyConsent::place(m_place);
        AppConfig        *config = wxGetApp().app_config;
        const bool        valid  = config && where && (choice == "share" || choice == "decline");
        if (!valid) {
            BOOST_LOG_TRIVIAL(warning) << "privacy: the privacy page sent an incomplete choice; nothing saved";
            reply = reply_js({{"command", "privacy_saved"}, {"ok", false}});
            return true;
        }
        const PrivacyConsent::Record record =
            PrivacyConsent::make_choice(where->region, where->country, choice == "share", j.value("kept_preselection", false), m_choice_source,
                                        PrivacyConsent::iso8601_now(), PrivacyConsent::legal_approvals(),
                                        choice == "share" && j.value("program", false));
        // Saved first, applied second: diagnostics only ever start from a choice that is on disk.
        PrivacyConsent::save(*config, record);
        if (m_privacy_only)
            config->set("region", where->guide_region); // the wizard writes it itself on Finish
        config->save();
        UsageTelemetry::refresh(*config);
        m_saved = true;

        PrivacyChoiceStats::Event e =
            stats_event(m_choice_source == "preferences" ? "privacy_preference_changed" : "privacy_choice_submitted");
        e.choice           = record.enabled() ? "accepted" : "declined";
        e.selection_method = record.selection_method;
        m_stats.choice_made(e);

        BOOST_LOG_TRIVIAL(info) << "privacy: choice saved (" << record.diagnostics_preference << ", " << record.selection_method
                                << (record.improvement_program ? ", improvement program" : "") << ", " << m_choice_source << ", "
                                << where->id << ")";
        reply = reply_js({{"command", "privacy_saved"}, {"ok", true}});
        return true;
    }
    if (cmd == "privacy_open_policy") {
        open_policy();
        return true;
    }
    if (cmd == "privacy_finish") {
        m_finished = true;
        return true;
    }
    return false;
}

wxString PrivacyFlow::policy_file()
{
    // A translation is shipped only once counsel has approved it (fr-CA for Quebec, es-MX for
    // Mexico); until then everyone gets the English text.
    const fs::path dir  = fs::path(resources_dir()) / "web" / "privacy";
    std::string    lang = into_u8(wxGetApp().current_language_code_safe()); // "fr_CA", "es", ...
    std::vector<std::string> candidates;
    if (!lang.empty()) {
        std::replace(lang.begin(), lang.end(), '_', '-');
        candidates.push_back("slicer-privacy-policy." + lang + ".html");
        candidates.push_back("slicer-privacy-policy." + lang.substr(0, 2) + ".html");
    }
    candidates.push_back("slicer-privacy-policy.html");
    boost::system::error_code ec;
    for (const std::string &name : candidates)
        if (fs::exists(dir / name, ec))
            return from_u8((dir / name).make_preferred().string());
    return from_u8((dir / "slicer-privacy-policy.html").make_preferred().string());
}

void PrivacyFlow::open_policy()
{
    wxString path = policy_file();
#ifdef _WIN32
    path.Replace("\\", "/");
    wxLaunchDefaultBrowser("file:///" + path);
#else
    wxLaunchDefaultBrowser("file://" + path);
#endif
}

PrivacySetupDialog::PrivacySetupDialog(wxWindow *parent, const std::string &choice_source)
    : DPIDialog(parent, wxID_ANY, _L("Slicer Privacy Policy"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU | wxRESIZE_BORDER)
    , m_flow(true, choice_source)
{
    SetBackgroundColour(*wxWHITE);
    wxString url = from_u8((fs::path(resources_dir()) / "web" / "guide" / "11" / "index.html").make_preferred().string());
    url          = "file://" + url + "?mode=privacy";
    const wxString lang = wxGetApp().current_language_code_safe();
    if (!lang.empty())
        url += "&lang=" + lang;

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    m_browser   = WebView::CreateWebView(this, url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "privacy: could not create the web view for the privacy dialog";
        return;
    }
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);
    SetSize(FromDIP(wxSize(820, 660)));
    CenterOnParent();

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrivacySetupDialog::on_script_message, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrivacySetupDialog::on_new_window, this, m_browser->GetId());
    wxGetApp().UpdateDlgDarkUI(this);
}

PrivacySetupDialog::~PrivacySetupDialog()
{
    if (m_browser) {
        delete m_browser;
        m_browser = nullptr;
    }
}

void PrivacySetupDialog::on_script_message(wxWebViewEvent &evt)
{
    json j = json::parse(evt.GetString().utf8_string(), nullptr, false);
    if (!j.is_object())
        return;
    std::string reply;
    if (m_flow.handle(j, reply) && !reply.empty()) {
        const wxString js = wxString::FromUTF8(reply);
        CallAfter([this, js] { WebView::RunScript(m_browser, js); });
    }
    if (m_flow.finished() && IsModal())
        CallAfter([this] { EndModal(m_flow.choice_saved() ? wxID_OK : wxID_CANCEL); });
}

void PrivacySetupDialog::on_new_window(wxWebViewEvent &evt) { wxLaunchDefaultBrowser(evt.GetURL()); }

bool run_privacy_setup(wxWindow *parent, const std::string &choice_source)
{
    PrivacySetupDialog dlg(parent, choice_source);
    dlg.ShowModal();
    return dlg.choice_saved();
}

}} // namespace Slic3r::GUI
