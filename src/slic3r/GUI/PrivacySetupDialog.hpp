#ifndef slic3r_GUI_PrivacySetupDialog_hpp_
#define slic3r_GUI_PrivacySetupDialog_hpp_

#include <memory>
#include <string>

#include <wx/dialog.h>
#include <wx/string.h>

#include <nlohmann/json.hpp>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/PrivacyChoiceStats.hpp"

class wxWebView;
class wxWebViewEvent;

namespace Slic3r { namespace GUI {

// The privacy steps of setup (0.1.6), shared by the setup wizard (GuideFrame) and
// PrivacySetupDialog: the one-click region page (resources/web/guide/11: United States, Canada,
// Mexico, ...; each place decides the privacy rules) and the "Slicer Privacy Policy" page (guide 13). The pages send their messages here;
// this class keeps what was chosen, saves the record (PrivacyConsent) the moment the person
// presses Continue, applies it to usage diagnostics at once, and counts the page and the decision
// for the privacy-choice statistics (disabled in this release, see PrivacyChoiceStats).
class PrivacyFlow
{
public:
    // `privacy_only`: the privacy steps alone (PrivacySetupDialog), not the whole setup wizard.
    // `choice_source`: "initial_setup", "update_prompt" or "preferences".
    PrivacyFlow(bool privacy_only, std::string choice_source);
    ~PrivacyFlow();

    // A message from a guide page. Returns true when it was one of the privacy steps' messages;
    // `reply` is then JavaScript to run in the page (possibly empty).
    bool handle(const nlohmann::json &message, std::string &reply);

    // The printer-profile region of the place chosen on the region page ("North America"), for the
    // wizard's own region setting; empty until a place is chosen.
    std::string guide_region() const;

    // The person pressed Continue on the privacy page and the choice was saved.
    bool choice_saved() const { return m_saved; }
    // The privacy-only flow is done (its page asked to close).
    bool finished() const { return m_finished; }

    // The full policy, in the person's language when there is a reviewed translation.
    static wxString policy_file();
    static void     open_policy();

private:
    PrivacyChoiceStats::Event stats_event(const char *type) const;

    bool        m_privacy_only;
    std::string m_choice_source;
    std::string m_place; // PrivacyConsent::Place id chosen on the region page ("ca")
    bool        m_saved{false};
    bool        m_finished{false};
    PrivacyChoiceStats::FlowCounter m_stats;
};

// The privacy steps on their own, for an installation that has no valid choice yet (an update
// from 0.1.5 or earlier, a setup that was closed early) and for Preferences > Online (changing the
// country, or choosing for the first time). Returns true when a choice was saved; closing the
// window saves nothing, and nothing is collected until a choice exists.
bool run_privacy_setup(wxWindow *parent, const std::string &choice_source);

class PrivacySetupDialog : public DPIDialog
{
public:
    PrivacySetupDialog(wxWindow *parent, const std::string &choice_source);
    ~PrivacySetupDialog() override;

    bool choice_saved() const { return m_flow.choice_saved(); }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void on_script_message(wxWebViewEvent &evt);
    void on_new_window(wxWebViewEvent &evt);

    wxWebView  *m_browser{nullptr};
    PrivacyFlow m_flow;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PrivacySetupDialog_hpp_
