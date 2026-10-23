#ifndef slic3r_UnlayeredAccount_hpp_
#define slic3r_UnlayeredAccount_hpp_

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r {

// The Unlayered account the application itself is signed in as, for requests made by native code -
// today the in-app update download, which unlayered3d.com only serves to accounts with software
// access.
//
// This is NOT the Universe tab's sign-in. That one lives in the web view's cookie jar as an httpOnly
// cookie that neither the page nor this code can read, and native HTTP sends no cookies anyway. So
// the application signs in a second time through the same browser handoff the tab uses (the person
// confirms "Sign in the slicer" in their own browser) and collects a device token instead of a
// cookie:
//
//   1. start_handoff()   POST /api/login/handoff/start            -> {id, secret}
//   2. the caller opens  <site>/auth/slicer?id=<id>                 in the default browser
//   3. collect()         POST /api/slicer/device/collect (polled)  -> {token, account}
//
// The token is kept in the operating system's credential store (Windows Credential Manager, the
// macOS keychain, libsecret), never in the plain-text config file, and is sent only to the site it
// came from, as `Authorization: Bearer`. It is revoked by signing out here, by signing out on the
// website (which ends every session for the account), or after 180 days unused.
//
// All network calls are synchronous; call them from a worker thread. The object is thread safe.
class UnlayeredAccount
{
public:
    struct Profile
    {
        std::string name;
        std::string email;
        bool        software_access{false};
    };

    struct Handoff
    {
        std::string id;
        std::string secret;
        std::string browser_url; // where to send the person to confirm
    };

    enum class CollectResult { Pending, SignedIn, Failed };
    enum class RefreshResult { SignedIn, SignedOut, Unreachable };

    // A signed link to an update installer, from /api/slicer/update/download.
    struct DownloadLink
    {
        std::string url; // stands on its own; fetch it WITHOUT credentials
        std::string filename;
        std::string sha256;
        uint64_t    size{0};
        std::string version;
    };

    // site_url: e.g. "https://unlayered3d.com", no trailing slash required.
    explicit UnlayeredAccount(std::string site_url);

    const std::string &site_url() const { return m_site; }

    // Reads a stored token, if any. Cheap; no network.
    void load();

    bool                   signed_in() const;
    std::optional<Profile> profile() const;

    // The stored device token, empty when signed out. For the printer-update feeds, which take it
    // inside a FeedCredential bound to site_url() and attach it only to requests on that site;
    // nothing may send it anywhere without that check.
    std::string device_token() const { return bearer(); }

    // Step 1 of signing in.
    bool start_handoff(Handoff &out, std::string &error) const;
    // Step 3, polled every couple of seconds while the person confirms in their browser. On
    // SignedIn the token is stored and profile() is filled.
    CollectResult collect(const Handoff &handoff, std::string &error);

    // Confirms the stored token still works and refreshes profile(). SignedOut means the site
    // refused it (expired, revoked, signed out on the website) and it has been forgotten.
    RefreshResult refresh();

    // Forgets the token here and revokes it on the site (best effort; the local half always
    // happens).
    void sign_out();

    // Exchanges the stored token for a signed installer link. `route_url` is the manifest's
    // `unlayered_downloads[].url`, which must be on this account's site - the token is never sent
    // anywhere else. On a 401 the token is forgotten, as in refresh().
    bool fetch_download_link(const std::string &route_url, DownloadLink &out, unsigned &http_status,
                             std::string &error);

    // "https://unlayered3d.com" -> "unlayered3d.com", "http://localhost:3111" -> "localhost:3111";
    // used to keep the token on its own host and port.
    static std::string host_of(const std::string &url);

private:
    std::string bearer() const;
    void        store_token(const std::string &token, const Profile &profile);
    void        forget_token();
    std::string secret_service() const;

    const std::string      m_site;
    mutable std::mutex     m_mutex;
    std::string            m_token;
    std::optional<Profile> m_profile;
};

} // namespace Slic3r

#endif // slic3r_UnlayeredAccount_hpp_
