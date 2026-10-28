#ifndef slic3r_Utils_PrivacyChoiceStats_hpp_
#define slic3r_Utils_PrivacyChoiceStats_hpp_

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {

// How many people see the privacy page, and what they choose (0.1.6). Separate from usage
// diagnostics in every way: no install id, no session id, no slicer activity, only the
// dimensions below, meant to be added into server-side counters and never stored per person.
//
// DISABLED in this release. Nothing is transmitted unless counsel has approved a legal basis
// (PrivacyConsent::LegalApprovals::privacy_choice_stats) AND an endpoint exists (ENDPOINT below,
// empty: no backend is deployed). Until then every event stops at the adapter, which only logs it.
// The organisation-wide switch UNLAYERED_NO_TELEMETRY stops it too.
//
// Counting, not tracking: events are de-duplicated with in-memory keys that die with the setup
// flow or the process, never with an identifier written to disk. The counts are events
// (impressions, decisions), not unique people.
namespace PrivacyChoiceStats {

// No backend yet. Fill in only together with the legal approval.
constexpr const char *ENDPOINT = "";

struct Event
{
    std::string event_type;       // "privacy_page_shown" / "privacy_choice_submitted" / "privacy_preference_changed"
    std::string region;           // region code
    std::string country;          // ISO code or empty
    std::string page_version;     // "privacy-na-v1"
    std::string notice_version;   // PrivacyConsent::NOTICE_VERSION
    std::string choice;           // "accepted" / "declined"; empty for privacy_page_shown
    std::string choice_source;    // "initial_setup" / "preferences" / "update_prompt"
    std::string selection_method; // "explicit" / "preselected_confirmed"; empty for privacy_page_shown
};

// The event as the schema allows it, or nullopt when any field is missing, unknown or out of range.
std::optional<nlohmann::json> to_body(const Event &event);
// The same check for a body from anywhere (what the server applies): exactly the approved fields.
bool valid_body(const nlohmann::json &body);

// May anything be transmitted? All three must hold.
bool transmission_allowed(bool legal_basis_approved, const std::string &endpoint, bool organization_off);

// De-duplication for one flow (one run of the setup wizard or of the privacy dialog): each page
// impression is counted once however often the page re-renders or the person navigates back, and
// the decision once, when the flow ends, with the choice that stands.
class FlowCounter
{
public:
    using Sink = std::function<void(const nlohmann::json &body)>;
    explicit FlowCounter(Sink sink) : m_sink(std::move(sink)) {}

    void page_shown(const Event &event);
    // The person's latest choice in this flow; replaces an earlier one. Sent by finish().
    void choice_made(const Event &event);
    // Sends the decision (once). Further calls do nothing.
    void finish();

private:
    Sink                  m_sink;
    std::set<std::string> m_shown;
    std::optional<Event>  m_choice;
    bool                  m_finished{false};
};

// The app's sink: transmits in the background when allowed, otherwise logs and drops. Never
// blocks and never fails the caller; a failed send is not retried.
void submit(const nlohmann::json &body);

} // namespace PrivacyChoiceStats
} // namespace Slic3r

#endif // slic3r_Utils_PrivacyChoiceStats_hpp_
