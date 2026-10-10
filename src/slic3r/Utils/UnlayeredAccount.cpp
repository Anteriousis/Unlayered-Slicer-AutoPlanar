#include "UnlayeredAccount.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <wx/secretstore.h>
#include <wx/utils.h>

#include "libslic3r_version.h"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {

namespace {

std::string user_agent() { return std::string("UnlayeredSlicer/") + UNLAYERED_VERSION; }

// Shown on the site next to the token, so a person can tell their machines apart.
std::string device_label()
{
    return (boost::format("Unlayered Slicer %1% on %2%") % UNLAYERED_VERSION % wxGetHostName().ToUTF8().data()).str();
}

std::string trim_slash(std::string url)
{
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

// Every request in this file carries, or is about to receive, a credential, so certificate
// verification is on for all of them even though the shared Http client defaults it off.
Http prepared(Http http)
{
    http.tls_verify(true)
        .timeout_connect(10)
        .timeout_max(30)
        .size_limit(64 * 1024)
        .header("User-Agent", user_agent())
        .header("Accept", "application/json");
    return http;
}

std::string error_from_body(const std::string &body, const std::string &fallback)
{
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_object() && j.contains("error") && j["error"].is_string())
            return j["error"].get<std::string>();
    } catch (...) {}
    return fallback;
}

} // namespace

UnlayeredAccount::UnlayeredAccount(std::string site_url) : m_site(trim_slash(std::move(site_url))) {}

std::string UnlayeredAccount::host_of(const std::string &url)
{
    // Host AND port: a different port on the same host is a different server, and the token must
    // not be handed to it.
    std::string port;
    std::string host = Http::get_host_from_url(url, &port);
    boost::algorithm::to_lower(host);
    return port.empty() ? host : host + ":" + port;
}

std::string UnlayeredAccount::secret_service() const
{
    // Per host, so a token minted by a local test server never replaces (or is sent instead of) the
    // real one.
    return "Unlayered Slicer account (" + host_of(m_site) + ")";
}

void UnlayeredAccount::load()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return;
    wxString      username;
    wxSecretValue secret;
    if (!store.Load(wxString::FromUTF8(secret_service()), username, secret) || !secret.IsOk())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_token.assign(static_cast<const char *>(secret.GetData()), secret.GetSize());
    // The email is kept as the credential's user name, so the menus can show who is signed in
    // before refresh() has reached the site - or when it cannot.
    Profile p;
    p.email = username.ToUTF8().data();
    p.name  = p.email;
    m_profile = p;
}

bool UnlayeredAccount::signed_in() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_token.empty();
}

std::optional<UnlayeredAccount::Profile> UnlayeredAccount::profile() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_token.empty() ? std::nullopt : m_profile;
}

std::string UnlayeredAccount::bearer() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_token;
}

void UnlayeredAccount::store_token(const std::string &token, const Profile &profile)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token   = token;
        m_profile = profile;
    }
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk() ||
        !store.Save(wxString::FromUTF8(secret_service()), wxString::FromUTF8(profile.email),
                    wxSecretValue(token.size(), token.data()))) {
        // Deliberately no plain-text fallback: the person stays signed in for this session only and
        // is asked again next launch, which is a far smaller cost than a credential in a config file.
        BOOST_LOG_TRIVIAL(warning) << "account: the system credential store is unavailable; "
                                      "staying signed in for this session only";
    }
}

void UnlayeredAccount::forget_token()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token.clear();
        m_profile.reset();
    }
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk())
        store.Delete(wxString::FromUTF8(secret_service()));
}

bool UnlayeredAccount::start_handoff(Handoff &out, std::string &error) const
{
    bool ok = false;
    prepared(Http::post(m_site + "/api/login/handoff/start"))
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .on_complete([&](std::string body, unsigned) {
            try {
                auto j = nlohmann::json::parse(body);
                out.id     = j.at("id").get<std::string>();
                out.secret = j.at("secret").get<std::string>();
                out.browser_url = m_site + "/auth/slicer?id=" + Http::url_encode(out.id);
                ok              = !out.id.empty() && !out.secret.empty();
                if (!ok)
                    error = "The sign-in service sent an incomplete answer.";
            } catch (const std::exception &e) {
                error = std::string("The sign-in service sent an unexpected answer: ") + e.what();
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            error = status ? error_from_body(body, (boost::format("HTTP %1%") % status).str()) : err;
        })
        .perform_sync();
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "account: could not start sign-in: " << error;
    return ok;
}

UnlayeredAccount::CollectResult UnlayeredAccount::collect(const Handoff &handoff, std::string &error)
{
    CollectResult result = CollectResult::Failed;
    std::string   token;
    Profile       profile;

    const nlohmann::json request = {{"id", handoff.id}, {"secret", handoff.secret}, {"label", device_label()}};

    prepared(Http::post(m_site + "/api/slicer/device/collect"))
        .header("Content-Type", "application/json")
        .set_post_body(request.dump())
        .on_complete([&](std::string body, unsigned) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.value("pending", false)) {
                    result = CollectResult::Pending;
                    return;
                }
                token = j.at("token").get<std::string>();
                if (auto acct = j.find("account"); acct != j.end() && acct->is_object()) {
                    profile.name  = acct->value("name", "");
                    profile.email = acct->value("email", "");
                }
                result = token.empty() ? CollectResult::Failed : CollectResult::SignedIn;
            } catch (const std::exception &e) {
                error = std::string("The sign-in service sent an unexpected answer: ") + e.what();
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            if (status == 0) {
                // No answer at all (Wi-Fi blip, sleep). The handoff is still waiting on the site,
                // so keep polling rather than make the person start over.
                BOOST_LOG_TRIVIAL(info) << "account: sign-in poll got no answer, retrying: " << err;
                result = CollectResult::Pending;
                return;
            }
            error = error_from_body(body, (boost::format("HTTP %1%") % status).str());
        })
        .perform_sync();

    if (result == CollectResult::SignedIn) {
        store_token(token, profile);
        BOOST_LOG_TRIVIAL(info) << "account: signed in as " << profile.email;
    }
    return result;
}

UnlayeredAccount::RefreshResult UnlayeredAccount::refresh()
{
    const std::string token = bearer();
    if (token.empty())
        return RefreshResult::SignedOut;

    RefreshResult result = RefreshResult::Unreachable;
    Profile       p;
    prepared(Http::get(m_site + "/api/slicer/device/me"))
        .header("Authorization", "Bearer " + token)
        .on_complete([&](std::string body, unsigned) {
            try {
                auto j            = nlohmann::json::parse(body);
                p.name            = j.value("name", "");
                p.email           = j.value("email", "");
                p.software_access = j.value("softwareAccess", false);
                result            = RefreshResult::SignedIn;
            } catch (...) {
                // A login page or proxy error with a 200: say nothing about the token.
            }
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            if (status == 401)
                result = RefreshResult::SignedOut;
            else
                BOOST_LOG_TRIVIAL(info) << "account: could not reach the site to check the sign-in: "
                                        << (status ? std::to_string(status) : err);
        })
        .perform_sync();

    if (result == RefreshResult::SignedIn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_token.empty())
            m_profile = p;
    } else if (result == RefreshResult::SignedOut) {
        BOOST_LOG_TRIVIAL(info) << "account: the site no longer accepts the stored sign-in; forgetting it";
        forget_token();
    }
    return result;
}

void UnlayeredAccount::sign_out()
{
    const std::string token = bearer();
    forget_token();
    if (token.empty())
        return;
    // Fire and forget on a background thread: signing out must never wait on the network, and the
    // local half - the part that matters if this machine is the one being handed on - is done.
    auto http = prepared(Http::post(m_site + "/api/slicer/device/signout"));
    http.header("Authorization", "Bearer " + token)
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .on_error([](std::string, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "account: server-side sign-out failed (" << (status ? std::to_string(status) : err)
                                    << "); the token was forgotten locally";
        });
    http.perform();
}

bool UnlayeredAccount::fetch_download_link(const std::string &route_url, DownloadLink &out, unsigned &http_status,
                                           std::string &error)
{
    http_status = 0;
    // The manifest is unauthenticated. If it ever named another host, sending the token there
    // would hand the account to whoever controls that host - so it is never sent.
    if (host_of(route_url) != host_of(m_site)) {
        error = "The update manifest points the download at " + host_of(route_url) + ", which is not " +
                host_of(m_site) + ". Refusing to send the account sign-in there.";
        BOOST_LOG_TRIVIAL(warning) << "updater: " << error;
        return false;
    }
    const std::string token = bearer();
    if (token.empty()) {
        http_status = 401;
        error       = "Not signed in.";
        return false;
    }

    bool ok = false;
    prepared(Http::get(route_url))
        .header("Authorization", "Bearer " + token)
        .on_complete([&](std::string body, unsigned status) {
            http_status = status;
            try {
                auto j       = nlohmann::json::parse(body);
                out.url      = j.at("url").get<std::string>();
                out.filename = j.value("filename", "");
                out.sha256   = j["sha256"].is_string() ? j["sha256"].get<std::string>() : std::string();
                out.size     = j.value("size", uint64_t(0));
                out.version  = j.value("version", "");
                ok           = !out.url.empty();
            } catch (const std::exception &e) {
                error = std::string("The download service sent an unexpected answer: ") + e.what();
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_status = status;
            error       = status ? error_from_body(body, (boost::format("HTTP %1%") % status).str()) : err;
        })
        .perform_sync();

    if (http_status == 401)
        forget_token();
    return ok;
}

} // namespace Slic3r
