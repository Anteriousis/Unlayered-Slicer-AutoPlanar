#include "PrivacyConsent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <regex>
#include <set>

#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"

using json = nlohmann::json;

namespace Slic3r {
namespace PrivacyConsent {

const LegalApprovals &legal_approvals()
{
    // A flag is only turned on once counsel has approved it; see docs/privacy/LEGAL_REVIEW_CHECKLIST.md.
    // Changing one is a code change, reviewed like any other.
    //   us_preselect_share: approved by counsel per Alex, 2026-10-14 (written confirmation pending, A1).
    static const LegalApprovals approvals{/* us_preselect_share */ true, /* privacy_choice_stats */ false};
    return approvals;
}

namespace {

const std::vector<std::pair<const char *, const char *>> &guide_regions()
{
    // The setup wizard's region page (resources/web/guide/11) and the codes records keep.
    static const std::vector<std::pair<const char *, const char *>> regions = {
        {"North America", "north_america"}, {"Europe", "europe"}, {"Asia-Pacific", "asia_pacific"},
        {"China", "china"},                 {"Others", "other"},
    };
    return regions;
}

// EU and EEA member states, and the EU's outermost regions that have their own ISO codes. Three of
// those are in the Caribbean and so on the "Other North America" list: the GDPR applies there.
const std::set<std::string> &eu_eea()
{
    static const std::set<std::string> codes = {
        "AT", "BE", "BG", "HR", "CY", "CZ", "DK", "EE", "FI", "FR", "DE", "GR", "HU", "IE", "IT",
        "LV", "LT", "LU", "MT", "NL", "PL", "PT", "RO", "SK", "SI", "ES", "SE", // EU
        "IS", "LI", "NO",                                                       // EEA
        "GP", "MQ", "MF", "GF", "RE", "YT",                                     // outermost regions
    };
    return codes;
}

const char *region_short(const std::string &region)
{
    if (region == "north_america") return "na";
    if (region == "europe")        return "eu";
    if (region == "asia_pacific")  return "apac";
    if (region == "china")         return "cn";
    return "intl";
}

bool is_country_code(const std::string &s)
{
    return s.size() == 2 && std::isupper(static_cast<unsigned char>(s[0])) && std::isupper(static_cast<unsigned char>(s[1]));
}

bool is_iso8601_utc(const std::string &s)
{
    static const std::regex re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
    return std::regex_match(s, re);
}

bool one_of(const std::string &s, std::initializer_list<const char *> values)
{
    return std::any_of(values.begin(), values.end(), [&s](const char *v) { return s == v; });
}

} // namespace

std::string region_code(const std::string &guide_region)
{
    for (const auto &[label, code] : guide_regions())
        if (guide_region == label || guide_region == code)
            return code;
    return {};
}

bool is_region_code(const std::string &code)
{
    for (const auto &entry : guide_regions())
        if (code == entry.second)
            return true;
    return false;
}

const char *jurisdiction_key(Jurisdiction j)
{
    switch (j) {
    case Jurisdiction::UnitedStates:      return "us";
    case Jurisdiction::Canada:            return "ca";
    case Jurisdiction::OtherNorthAmerica: return "other_na";
    case Jurisdiction::EuEea:             return "eu_eea";
    case Jurisdiction::UnitedKingdom:     return "uk";
    case Jurisdiction::Switzerland:       return "ch";
    case Jurisdiction::Unknown:           break;
    }
    return "unknown";
}

const std::vector<Country> &other_north_america()
{
    // Mexico, Central America, the Caribbean and the rest of the continent. Each is its own
    // jurisdiction: the list exists so a choice names one, never to treat them alike.
    static const std::vector<Country> countries = {
        {"MX", "Mexico"},
        {"BZ", "Belize"},
        {"CR", "Costa Rica"},
        {"SV", "El Salvador"},
        {"GT", "Guatemala"},
        {"HN", "Honduras"},
        {"NI", "Nicaragua"},
        {"PA", "Panama"},
        {"AI", "Anguilla"},
        {"AG", "Antigua and Barbuda"},
        {"AW", "Aruba"},
        {"BS", "Bahamas"},
        {"BB", "Barbados"},
        {"BM", "Bermuda"},
        {"BQ", "Bonaire, Sint Eustatius and Saba"},
        {"VG", "British Virgin Islands"},
        {"KY", "Cayman Islands"},
        {"CU", "Cuba"},
        {"CW", "Cura\xC3\xA7" "ao"},
        {"DM", "Dominica"},
        {"DO", "Dominican Republic"},
        {"GD", "Grenada"},
        {"GP", "Guadeloupe"},
        {"HT", "Haiti"},
        {"JM", "Jamaica"},
        {"MQ", "Martinique"},
        {"MS", "Montserrat"},
        {"PR", "Puerto Rico"},
        {"BL", "Saint Barth\xC3\xA9lemy"},
        {"KN", "Saint Kitts and Nevis"},
        {"LC", "Saint Lucia"},
        {"MF", "Saint Martin (French part)"},
        {"VC", "Saint Vincent and the Grenadines"},
        {"SX", "Sint Maarten (Dutch part)"},
        {"TT", "Trinidad and Tobago"},
        {"TC", "Turks and Caicos Islands"},
        {"VI", "U.S. Virgin Islands"},
        {"GL", "Greenland"},
        {"PM", "Saint Pierre and Miquelon"},
    };
    return countries;
}

const std::vector<Place> &places()
{
    // In the order the region page lists them.
    static const std::vector<Place> list = {
        {"us", "United States", "North America", "north_america", "US"},
        {"ca", "Canada", "North America", "north_america", "CA"},
        {"mx", "Mexico", "North America", "north_america", "MX"},
        {"central_america_caribbean", "Central America & Caribbean", "North America", "north_america", ""},
        {"eu_eea", "European Union / EEA", "Europe", "europe", ""},
        {"uk", "United Kingdom", "Europe", "europe", "GB"},
        {"ch", "Switzerland", "Europe", "europe", "CH"},
        {"asia_pacific", "Asia-Pacific", "Asia-Pacific", "asia_pacific", ""},
        {"china", "China", "China", "china", ""},
        {"other", "Rest of the world", "Others", "other", ""},
    };
    return list;
}

const Place *place(const std::string &id)
{
    for (const Place &p : places())
        if (id == p.id)
            return &p;
    return nullptr;
}

const Place *place_of(const std::string &region, const std::string &country)
{
    for (const Place &p : places())
        if (region == p.region && country == p.country)
            return &p;
    return nullptr;
}

bool is_north_america_country(const std::string &code)
{
    if (code == "US" || code == "CA")
        return true;
    const auto &list = other_north_america();
    return std::any_of(list.begin(), list.end(), [&code](const Country &c) { return code == c.code; });
}

RegionPolicy policy_for(const std::string &region, const std::string &country, const LegalApprovals &approvals)
{
    RegionPolicy p;
    if (const Place *where = place_of(region, country)) {
        p.consent_ui_version = std::string(where->id) + "-v1";
        p.page_version       = std::string("privacy-") + where->id + "-v1";
    } else {
        p.consent_ui_version = (is_region_code(region) ? region : std::string("unknown")) + "-v1";
        p.page_version       = std::string("privacy-") + region_short(region) + "-v1";
    }

    if (region == "north_america" && country.empty()) {
        // "Central America & Caribbean": one choice for many jurisdictions (Alex 2026-10-13, one-click
        // region page). The strict reading applies; which laws exactly is on the legal review list.
        p.jurisdiction = Jurisdiction::OtherNorthAmerica;
    } else if (region == "north_america") {
        if (country == "US") {
            p.jurisdiction    = Jurisdiction::UnitedStates;
            // The only place a preselected "share" may ever exist, and only with sign-off.
            p.preselect_share = approvals.us_preselect_share;
            p.pending_review  = !approvals.us_preselect_share;
        } else if (country == "CA") {
            p.jurisdiction   = Jurisdiction::Canada;
            p.pending_review = false; // affirmative choice: the strict reading, nothing to wait for
        } else if (is_north_america_country(country)) {
            p.jurisdiction = eu_eea().count(country) ? Jurisdiction::EuEea : Jurisdiction::OtherNorthAmerica;
        }
    } else if (region == "europe") {
        // The region page does not ask for a European country, so take the strictest reading.
        p.jurisdiction = Jurisdiction::EuEea;
        if (country == "GB")
            p.jurisdiction = Jurisdiction::UnitedKingdom;
        else if (country == "CH")
            p.jurisdiction = Jurisdiction::Switzerland;
    }
    return p;
}

std::optional<Record> parse(const std::string &json_text)
{
    json j = json::parse(json_text, nullptr, false);
    if (!j.is_object())
        return std::nullopt;

    static const std::set<std::string> string_fields = {"region",         "country",        "diagnostics_preference",
                                                        "selection_method", "choice_source",  "policy_version",
                                                        "notice_version", "consent_ui_version", "choice_timestamp"};
    // "improvement_program" is optional: records written before notice 2026-10-v3 have none.
    const bool has_program = j.contains("improvement_program");
    if (j.size() != string_fields.size() + 1 + (has_program ? 1 : 0))
        return std::nullopt;
    for (const auto &[key, value] : j.items()) {
        if (key == "model_upload_permission" || key == "improvement_program") {
            if (!value.is_boolean())
                return std::nullopt;
        } else if (!string_fields.count(key) || !value.is_string())
            return std::nullopt;
    }
    if (!j.contains("model_upload_permission"))
        return std::nullopt;

    Record r;
    r.region                  = j["region"].get<std::string>();
    r.country                 = j["country"].get<std::string>();
    r.diagnostics_preference  = j["diagnostics_preference"].get<std::string>();
    r.selection_method        = j["selection_method"].get<std::string>();
    r.choice_source           = j["choice_source"].get<std::string>();
    r.policy_version          = j["policy_version"].get<std::string>();
    r.notice_version          = j["notice_version"].get<std::string>();
    r.consent_ui_version      = j["consent_ui_version"].get<std::string>();
    r.choice_timestamp        = j["choice_timestamp"].get<std::string>();
    r.model_upload_permission = j["model_upload_permission"].get<bool>();
    r.improvement_program     = has_program && j["improvement_program"].get<bool>();

    if (!one_of(r.diagnostics_preference, {"enabled", "disabled"}) ||
        !one_of(r.selection_method, {"explicit", "preselected_confirmed", "legacy_setting"}) ||
        !one_of(r.choice_source, {"initial_setup", "preferences", "update_prompt", "legacy_setting"}))
        return std::nullopt;
    // Upload permission is per model, never stored as a standing yes.
    if (r.model_upload_permission)
        return std::nullopt;
    // The program is part of sharing: a "no" can never carry it.
    if (r.improvement_program && !r.enabled())
        return std::nullopt;

    if (r.selection_method == "legacy_setting") {
        // Only an old "off" is ever migrated, and nothing about it was asked or timed.
        if (r.diagnostics_preference != "disabled" || r.choice_source != "legacy_setting" || !r.choice_timestamp.empty())
            return std::nullopt;
        if (!r.region.empty() && !is_region_code(r.region))
            return std::nullopt;
        return r;
    }

    if (r.choice_source == "legacy_setting" || !is_iso8601_utc(r.choice_timestamp) || r.policy_version.empty() ||
        r.notice_version.empty() || r.consent_ui_version.empty())
        return std::nullopt;
    // A "no" needs no region (switching diagnostics off in Preferences before ever choosing one);
    // a "yes" is always for a region. A North America country, when there is one, is one of its own.
    if (r.region.empty() ? (r.enabled() || !r.country.empty()) : !is_region_code(r.region))
        return std::nullopt;
    if (!r.country.empty() && (r.region == "north_america" ? !is_north_america_country(r.country) : !is_country_code(r.country)))
        return std::nullopt;
    if (r.selection_method == "preselected_confirmed" && r.diagnostics_preference != "enabled")
        return std::nullopt;
    return r;
}

std::string serialize(const Record &r)
{
    json j = {{"region", r.region},
              {"country", r.country},
              {"diagnostics_preference", r.diagnostics_preference},
              {"selection_method", r.selection_method},
              {"choice_source", r.choice_source},
              {"policy_version", r.policy_version},
              {"notice_version", r.notice_version},
              {"consent_ui_version", r.consent_ui_version},
              {"choice_timestamp", r.choice_timestamp},
              {"model_upload_permission", r.model_upload_permission},
              {"improvement_program", r.improvement_program && r.enabled()}};
    return j.dump();
}

std::optional<Record> load(const AppConfig &config)
{
    const std::string text = config.get(CONFIG_KEY);
    if (text.empty())
        return std::nullopt;
    std::optional<Record> r = parse(text);
    if (!r)
        BOOST_LOG_TRIVIAL(warning) << "privacy: the saved privacy choice is unreadable; treating it as never asked";
    return r;
}

void save(AppConfig &config, const Record &record)
{
    // Reported once per decision: saving the same answer again (Preferences reopened, the region
    // changed) reports nothing, an answer to new notice wording does, and a migrated pre-0.1.6
    // opt-out is not a decision made now.
    const std::optional<Record> previous = load(config);
    if (record.selection_method != "legacy_setting" &&
        (!previous || previous->diagnostics_preference != record.diagnostics_preference ||
         previous->notice_version != record.notice_version))
        config.set(CHOICE_REPORT_KEY, record.enabled() ? "shared" : "private");
    config.set(CONFIG_KEY, serialize(record));
    config.set_bool("usage_diagnostics", diagnostics_permitted(record, false, legal_approvals()));
}

void clear(AppConfig &config)
{
    config.set(CONFIG_KEY, "");
    config.set_bool("usage_diagnostics", false);
}

bool migrate_legacy_setting(AppConfig &config)
{
    // Once per installation: afterwards "usage_diagnostics" is only the mirror save() writes, and
    // reading it back as an old opt-out would turn "not asked yet" into "declined".
    if (config.get(MIGRATED_KEY) == "1")
        return false;
    config.set(MIGRATED_KEY, "1");
    if (!config.get(CONFIG_KEY).empty())
        return false;
    const std::string legacy = config.get("usage_diagnostics");
    if (legacy == "false" || legacy == "0") {
        Record r;
        r.region                 = region_code(config.get("region"));
        r.diagnostics_preference = "disabled";
        r.selection_method       = "legacy_setting";
        r.choice_source          = "legacy_setting";
        r.policy_version         = POLICY_VERSION;
        r.notice_version         = NOTICE_VERSION;
        r.consent_ui_version     = "legacy";
        save(config, r);
        return true;
    }
    // On by default before 0.1.6 is not a choice anyone made: nothing is sent until they make one.
    config.set_bool("usage_diagnostics", false);
    return false;
}

bool queue_earlier_choice_report(AppConfig &config)
{
    if (config.get(EARLIER_CHOICE_REPORTED_KEY) == "1")
        return false;
    config.set(EARLIER_CHOICE_REPORTED_KEY, "1");
    const std::optional<Record> record = load(config);
    if (!record || record->selection_method == "legacy_setting" || !config.get(CHOICE_REPORT_KEY).empty())
        return false;
    config.set(CHOICE_REPORT_KEY, record->enabled() ? "shared" : "private");
    return true;
}

bool organization_opt_out()
{
    const char *env = std::getenv("UNLAYERED_NO_TELEMETRY");
    return env && *env;
}

Status status(const std::optional<Record> &record, const LegalApprovals &approvals)
{
    if (!record)
        return Status::NotAsked;
    // A "no" always stands: through updates, new notice versions and country changes alike.
    if (!record->enabled())
        return Status::Declined;
    if (record->notice_version != NOTICE_VERSION)
        return Status::NeedsNewChoice;
    if (record->selection_method == "explicit")
        return Status::Granted;
    if (record->selection_method == "preselected_confirmed") {
        // Counts only where (and while) a preselected answer is allowed for this very country:
        // a US-style preselection never stands in for a Canadian affirmative choice.
        return policy_for(record->region, record->country, approvals).preselect_share ? Status::Granted : Status::NeedsNewChoice;
    }
    return Status::NeedsNewChoice;
}

bool diagnostics_permitted(const std::optional<Record> &record, bool organization_off, const LegalApprovals &approvals)
{
    return !organization_off && status(record, approvals) == Status::Granted;
}

bool diagnostics_permitted(const AppConfig &config)
{
    return diagnostics_permitted(load(config), organization_opt_out(), legal_approvals());
}

bool program_permitted(const std::optional<Record> &record, bool organization_off, const LegalApprovals &approvals)
{
    // Granted already means "this notice version"; a preselected Share never implies the program.
    return diagnostics_permitted(record, organization_off, approvals) && record->improvement_program;
}

bool program_permitted(const AppConfig &config)
{
    return program_permitted(load(config), organization_opt_out(), legal_approvals());
}

Record make_choice(const std::string &region, const std::string &country, bool share, bool confirmed_preselection,
                   const std::string &choice_source, const std::string &timestamp, const LegalApprovals &approvals,
                   bool program)
{
    const RegionPolicy policy = policy_for(region, country, approvals);
    Record r;
    r.region                 = region;
    r.country                = country;
    r.diagnostics_preference = share ? "enabled" : "disabled";
    // "Preselected, confirmed" only when there was a preselection to confirm and it was kept.
    r.selection_method   = share && confirmed_preselection && policy.preselect_share ? "preselected_confirmed" : "explicit";
    r.choice_source      = choice_source;
    r.policy_version     = POLICY_VERSION;
    r.notice_version     = NOTICE_VERSION;
    r.consent_ui_version = policy.consent_ui_version;
    r.choice_timestamp   = timestamp;
    r.improvement_program = share && program;
    return r;
}

std::string iso8601_now()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool request_complete(const ModelUploadRequest &q)
{
    return !q.file_label.empty() && !q.file_id.empty() && !q.included_data.empty() && !q.purpose.empty() &&
           !q.recipients.empty() && !q.retention.empty();
}

ModelUploadAuthorization ModelUploadAuthorization::granted(const ModelUploadRequest &request, bool ai_training_allowed)
{
    ModelUploadAuthorization a;
    if (!request_complete(request))
        return a; // an incomplete prompt cannot have been informed consent
    a.m_file_id     = request.file_id;
    // Training is only ever allowed when the prompt proposed it and the person ticked it.
    a.m_ai_training = request.ai_training_proposed && ai_training_allowed;
    a.m_used        = false;
    return a;
}

bool ModelUploadAuthorization::consume(const std::string &file_id)
{
    if (m_used || file_id.empty() || file_id != m_file_id)
        return false;
    m_used = true;
    return true;
}

} // namespace PrivacyConsent
} // namespace Slic3r
