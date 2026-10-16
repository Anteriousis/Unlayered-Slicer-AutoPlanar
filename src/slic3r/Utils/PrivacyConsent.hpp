#ifndef slic3r_Utils_PrivacyConsent_hpp_
#define slic3r_Utils_PrivacyConsent_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class AppConfig;

// The person's privacy choices and the rules that decide what they allow (0.1.6).
//
// One record in the app config ("privacy_consent", a JSON object) is the only source of truth for
// optional usage diagnostics (UsageTelemetry). The old "usage_diagnostics" bool is kept as a
// mirror for code and configs that read it, and is never read back as permission.
//
// Fail closed: no record, an unreadable record, a record for another notice version, or a choice
// that does not count as consent where the person lives all mean "do not collect". The
// organisation-wide switch UNLAYERED_NO_TELEMETRY beats every record.
//
// The regional rules live in one table (policy_for) and the legal sign-offs in one struct
// (LegalApprovals). Nothing outside this file should branch on a country.
//
// See PRIVACY.md at the repository root for the architecture and docs/privacy for the policy text
// and the legal review checklist.
namespace PrivacyConsent {

// Bump NOTICE_VERSION when the consent screen's text changes in a way that needs a new answer:
// records for any other notice version stop counting and the person is asked again.
constexpr const char *POLICY_VERSION = "2026-10-v3";
constexpr const char *NOTICE_VERSION = "2026-10-v3";

// App config key holding the record.
constexpr const char *CONFIG_KEY = "privacy_consent";
// Set once migrate_legacy_setting() has looked at a pre-0.1.6 config.
constexpr const char *MIGRATED_KEY = "privacy_consent_migrated";
// "private" or "shared": a choice the person just made, which the next update check reports once
// as ?choice= (GUI_App::check_new_version_sf). Anonymous by construction: with diagnostics off the
// check carries no install id, it never carries an account, and the site only adds one to a per-day
// counter of platform, version and choice. Cleared once the site has answered.
constexpr const char *CHOICE_REPORT_KEY = "update_check_report_choice";
// Set once queue_earlier_choice_report() has run on this installation.
constexpr const char *EARLIER_CHOICE_REPORTED_KEY = "update_check_report_choice_earlier";

// Decisions that belong to counsel, not to code. A flag can only be turned on by changing
// legal_approvals() in PrivacyConsent.cpp, in a reviewed commit: none of them is a user or
// app-config setting. us_preselect_share is ON since 2026-10-14 (counsel's approval per Alex).
struct LegalApprovals
{
    // United States: may the consent screen start with "Share usage diagnostics" selected? Off:
    // the US is asked like everywhere else, with nothing selected.
    bool us_preselect_share = false;
    // May privacy-choice statistics (PrivacyChoiceStats) be transmitted at all?
    bool privacy_choice_stats = false;
};
const LegalApprovals &legal_approvals();

// Regions as records store them: "north_america", "europe", "asia_pacific", "china", "other".
// region_code() also reads the printer-profile region the app config keeps ("North America").
std::string region_code(const std::string &guide_region); // "North America" -> "north_america"; unknown -> ""
bool        is_region_code(const std::string &code);

// The setup wizard's region page (Alex 2026-10-13): one click names the place, and the place
// decides the privacy rules. No separate country page. Each place is a record's region + country.
struct Place
{
    const char *id;           // "us", "ca", "mx", "central_america_caribbean", "eu_eea", "uk", "ch", "asia_pacific", "china", "other"
    const char *label;        // "United States", as the page shows it
    const char *guide_region; // the printer-profile region kept in the app config ("North America")
    const char *region;       // record region code
    const char *country;      // record country code; "" when the place is not one country
};
const std::vector<Place> &places();
const Place *place(const std::string &id); // nullptr when unknown
// The place a record's region + country describe, or nullptr.
const Place *place_of(const std::string &region, const std::string &country);

enum class Jurisdiction { UnitedStates, Canada, OtherNorthAmerica, EuEea, UnitedKingdom, Switzerland, Unknown };
const char *jurisdiction_key(Jurisdiction j); // "us", "ca", "other_na", "eu_eea", "uk", "ch", "unknown"

struct RegionPolicy
{
    Jurisdiction jurisdiction{Jurisdiction::Unknown};
    // "Share usage diagnostics" starts selected (only ever true with a legal approval).
    bool preselect_share{false};
    // The rules here have not had a country-specific review yet (shown nowhere; for reports).
    bool pending_review{true};
    // What the consent screen is called in records and statistics.
    std::string consent_ui_version; // "north_america-v1"
    std::string page_version;       // "privacy-na-v1"
};

// The rules for a region and (self-declared) country. Pure: the same inputs, the same answer.
RegionPolicy policy_for(const std::string &region, const std::string &country, const LegalApprovals &approvals);

// Countries and territories of North America other than the United States and Canada (ISO
// 3166-1 alpha-2) with their English names. Since the one-click region page, records hold "MX"
// or no country ("Central America & Caribbean"); the list still validates older records.
struct Country { const char *code; const char *name; };
const std::vector<Country> &other_north_america();
// A country code a North America record may hold: "US", "CA" or one of other_north_america().
bool is_north_america_country(const std::string &code);

// The record. Field names are the stored JSON names.
struct Record
{
    std::string region;                 // region code
    std::string country;                // ISO code; empty when the place is not one country
    std::string diagnostics_preference; // "enabled" / "disabled"
    std::string selection_method;       // "explicit" / "preselected_confirmed" / "legacy_setting"
    std::string choice_source;          // "initial_setup" / "preferences" / "update_prompt" / "legacy_setting"
    std::string policy_version;
    std::string notice_version;
    std::string consent_ui_version;
    std::string choice_timestamp; // ISO-8601 UTC, when the person chose; empty for a migrated setting
    // Uploading models is never allowed by this record: it is asked per model (ModelUploadConsent).
    // Stored so the record says so explicitly; always false in this version.
    bool model_upload_permission{false};
    // The "Slicing improvement program" box under "Share usage diagnostics" (notice 2026-10-v3):
    // the only consent that lets diagnostics be used for AI training. Never preselected; only ever
    // true together with "enabled". Optional on read: records from before v3 have no such field
    // and read as false, so an old decline stays a decline.
    bool improvement_program{false};

    bool enabled() const { return diagnostics_preference == "enabled"; }
};

// Strict: every field must be there with the right type and a known value, nothing else may be.
// Anything else is std::nullopt (and is treated as "never asked").
std::optional<Record> parse(const std::string &json_text);
std::string           serialize(const Record &record);

std::optional<Record> load(const AppConfig &config);
// Writes the record and the "usage_diagnostics" mirror. Does not save the config file. A new
// decision (not a carried-over pre-0.1.6 setting) also sets CHOICE_REPORT_KEY.
void save(AppConfig &config, const Record &record);
void clear(AppConfig &config);

// One time, at start-up: a config written before 0.1.6 has "usage_diagnostics" but no record.
// An explicit "false" (the person switched diagnostics off) becomes a "disabled" record, so the
// opt-out survives the update. Anything else (the old default "true") is NOT consent and leaves
// no record: the person is asked. Returns true when it wrote a record.
bool migrate_legacy_setting(AppConfig &config);

// One time, at start-up, after migrate_legacy_setting(): a decision made before the update check
// reported choices (0.1.6 itself) is queued for that report once, like a decision made now, so a
// "keep my data private" from 0.1.6 is counted when the person updates. Not a carried-over
// pre-0.1.6 opt-out, and nothing when no decision exists yet (save() reports that one when it is
// made). Returns true when it queued a report.
bool queue_earlier_choice_report(AppConfig &config);

// UNLAYERED_NO_TELEMETRY set to anything non-empty: optional diagnostics are off on this machine,
// whatever any record says.
bool organization_opt_out();

enum class Status {
    NotAsked,      // no valid record: ask; nothing is collected meanwhile
    Declined,      // a valid "disabled" record
    Granted,       // a valid "enabled" record that counts as consent where the person lives
    NeedsNewChoice // a record that no longer counts (other notice version, or a preselected answer
                   // in a place where only an explicit choice counts): ask again
};
Status status(const std::optional<Record> &record, const LegalApprovals &approvals);

// The one question every optional transmission asks. False unless status() is Granted and the
// organisation has not switched diagnostics off.
bool diagnostics_permitted(const std::optional<Record> &record, bool organization_off, const LegalApprovals &approvals);
bool diagnostics_permitted(const AppConfig &config);

// Membership of the Slicing improvement program: diagnostics permitted AND the program box ticked
// on the current notice. Withdrawing diagnostics, or the organisation switch, ends it too.
bool program_permitted(const std::optional<Record> &record, bool organization_off, const LegalApprovals &approvals);
bool program_permitted(const AppConfig &config);

// A record for a choice made now. `share`: which option was chosen. `confirmed_preselection`:
// share was chosen by leaving a preselected answer as it was (recorded as such, never as an
// explicit choice). `timestamp`: ISO-8601 UTC. `program`: the improvement-program box was ticked
// (it starts unticked, so this is always an explicit act); ignored unless `share`.
Record make_choice(const std::string &region, const std::string &country, bool share, bool confirmed_preselection,
                   const std::string &choice_source, const std::string &timestamp, const LegalApprovals &approvals,
                   bool program = false);

std::string iso8601_now();

// Model uploads (for support, troubleshooting, feedback) need their own answer, every time, for
// one model. Usage diagnostics consent never authorises one. The request says everything the
// prompt must show; the authorisation is single-use and only for the file it names.
struct ModelUploadRequest
{
    std::string file_label;      // what the person sees: the model's file name
    std::string file_id;         // what the upload code checks: e.g. a content hash
    std::string included_data;   // "The 3MF project: geometry, settings and thumbnails"
    std::string purpose;         // "Troubleshooting the crash you reported"
    bool        ai_training_proposed{false};
    std::string recipients;      // "Unlayered 3D, Inc. (hosted on ...)"
    std::string retention;       // "Deleted 90 days after the ticket is closed"
};

// Whether a request can be shown at all: every field the person must be told is filled in.
bool request_complete(const ModelUploadRequest &request);

class ModelUploadAuthorization
{
public:
    ModelUploadAuthorization() = default;
    // Only the upload prompt creates a granted one, after the person said yes to `request`.
    static ModelUploadAuthorization granted(const ModelUploadRequest &request, bool ai_training_allowed);
    // True once, for the file the person approved; every later call (or any other file) is false.
    bool consume(const std::string &file_id);
    bool ai_training_allowed() const { return m_ai_training; }

private:
    std::string m_file_id;
    bool        m_ai_training{false};
    bool        m_used{true};
};

} // namespace PrivacyConsent
} // namespace Slic3r

#endif // slic3r_Utils_PrivacyConsent_hpp_
