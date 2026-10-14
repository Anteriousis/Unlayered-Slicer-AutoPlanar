#include "PrivacyChoiceStats.hpp"

#include <cctype>
#include <regex>

#include <boost/log/trivial.hpp>

#include "Http.hpp"
#include "PrivacyConsent.hpp"

using json = nlohmann::json;

namespace Slic3r {
namespace PrivacyChoiceStats {

namespace {

bool one_of(const std::string &s, std::initializer_list<const char *> values)
{
    for (const char *v : values)
        if (s == v)
            return true;
    return false;
}

bool short_token(const std::string &s)
{
    static const std::regex re("^[a-z0-9][a-z0-9_.-]{0,39}$");
    return std::regex_match(s, re);
}

} // namespace

bool valid_body(const json &body)
{
    if (!body.is_object())
        return false;
    for (const auto &[key, value] : body.items())
        if (!one_of(key, {"event_type", "region", "country", "page_version", "notice_version", "choice", "choice_source",
                          "selection_method"}) ||
            !value.is_string())
            return false;

    auto field = [&body](const char *key) { return body.contains(key) ? body[key].get<std::string>() : std::string(); };
    const std::string type = field("event_type");
    if (!one_of(type, {"privacy_page_shown", "privacy_choice_submitted", "privacy_preference_changed"}))
        return false;
    if (!PrivacyConsent::is_region_code(field("region")))
        return false;
    const std::string country = field("country");
    if (!country.empty() && (field("region") == "north_america" ? !PrivacyConsent::is_north_america_country(country)
                                                                : !(country.size() == 2 && std::isupper((unsigned char) country[0]) &&
                                                                    std::isupper((unsigned char) country[1]))))
        return false;
    if (!short_token(field("page_version")) || !short_token(field("notice_version")))
        return false;
    if (!one_of(field("choice_source"), {"initial_setup", "preferences", "update_prompt"}))
        return false;

    if (type == "privacy_page_shown")
        return !body.contains("choice") && !body.contains("selection_method");
    return one_of(field("choice"), {"accepted", "declined"}) &&
           one_of(field("selection_method"), {"explicit", "preselected_confirmed"}) &&
           !(field("choice") == "declined" && field("selection_method") == "preselected_confirmed");
}

std::optional<json> to_body(const Event &e)
{
    json body = {{"event_type", e.event_type},
                 {"region", e.region},
                 {"country", e.country},
                 {"page_version", e.page_version},
                 {"notice_version", e.notice_version},
                 {"choice_source", e.choice_source}};
    if (e.event_type != "privacy_page_shown") {
        body["choice"]           = e.choice;
        body["selection_method"] = e.selection_method;
    }
    if (!valid_body(body))
        return std::nullopt;
    return body;
}

bool transmission_allowed(bool legal_basis_approved, const std::string &endpoint, bool organization_off)
{
    return legal_basis_approved && !endpoint.empty() && !organization_off;
}

void FlowCounter::page_shown(const Event &event)
{
    std::optional<json> body = to_body(event);
    if (!body)
        return;
    const std::string key = event.page_version + "|" + event.choice_source;
    if (!m_shown.insert(key).second)
        return; // a re-render or a Back: the same impression
    m_sink(*body);
}

void FlowCounter::choice_made(const Event &event)
{
    if (!m_finished && to_body(event))
        m_choice = event;
}

void FlowCounter::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_choice)
        if (std::optional<json> body = to_body(*m_choice))
            m_sink(*body);
}

void submit(const json &body)
{
    if (!valid_body(body))
        return;
    if (!transmission_allowed(PrivacyConsent::legal_approvals().privacy_choice_stats, ENDPOINT,
                              PrivacyConsent::organization_opt_out())) {
        BOOST_LOG_TRIVIAL(debug) << "privacy-choice statistics: not sent (disabled pending legal approval): " << body.dump();
        return;
    }
    // Fire and forget: setup never waits for, or depends on, this request.
    Http::post(ENDPOINT)
        .header("Content-Type", "application/json")
        .set_post_body(body.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .tls_verify(true)
        .on_error([](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "privacy-choice statistics: not delivered, status " << status << ", " << error;
        })
        .perform();
}

} // namespace PrivacyChoiceStats
} // namespace Slic3r
