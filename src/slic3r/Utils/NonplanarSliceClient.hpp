#ifndef slic3r_NonplanarSliceClient_hpp_
#define slic3r_NonplanarSliceClient_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Client for an EXTERNAL nonplanar slicing service: the plate goes out as a mesh, G-code comes
// back. The protocol is specified in NONPLANAR_SLICE_SERVICE.md and this file implements only
// that -- it names no service, and the address is configuration, exactly as a print host's is.
//
// Synchronous on purpose: every call is made from NonplanarSliceJob's worker thread, where
// blocking is the point, and Http::perform_sync() there keeps the control flow readable. Never
// call these from the GUI thread.
class NonplanarSliceClient
{
public:
    // The four status spellings the protocol defines.
    enum class Status { Queued, Running, Succeeded, Failed, Unknown };

    struct Job
    {
        std::string              id;
        Status                   status = Status::Unknown;
        std::string              status_text;   // as the service spelled it, for messages
        std::string              error;         // set when status == Failed
        std::string              filename;      // result.filename, if the service gave one
        std::string              sha256;
        uint64_t                 size = 0;
        std::vector<std::string> warnings;

        bool finished() const { return status == Status::Succeeded || status == Status::Failed; }
    };

    // ---- address + transport ------------------------------------------------------------
    // The configured service root without a trailing slash, or empty when none is configured.
    // ORCA_NONPLANAR_SLICE_URL wins over the stored setting and is never written back.
    static std::string configured_url();
    static bool        is_configured();
    // https anywhere; plain http only for a loopback host. Same rule as the web app tab, and
    // for the same reason: a cleartext address on the network is not offered an override.
    static bool        is_secure_url(const std::string &url);
    // Why `url` is unusable, as a sentence for the user; empty when it is fine.
    static std::string url_problem(const std::string &url);

    explicit NonplanarSliceClient(std::string url);

    // Each returns false and fills `error` with something worth showing a user.
    //
    // `settings` is the resolved print configuration as JSON -- the values, not preset names,
    // so a service that cannot see the user's preset library still slices correctly. The three
    // names go along as labels. An empty `settings` path sends none.
    bool submit(const boost::filesystem::path &mesh,
                const boost::filesystem::path &settings,
                const std::string &printer,
                const std::string &process,
                const std::string &filament,
                Job               &job,
                std::string       &error) const;

    bool poll(const std::string &job_id, Job &job, std::string &error) const;

    // Streams the result to `dest`. `progress` gets (received, total) and returns false to
    // abort; total is 0 when the service sends no Content-Length.
    using ProgressFn = std::function<bool(uint64_t /*received*/, uint64_t /*total*/)>;
    bool download_gcode(const std::string             &job_id,
                        const boost::filesystem::path &dest,
                        std::string                   &error,
                        ProgressFn                     progress = nullptr) const;

private:
    std::string m_url;   // no trailing slash

    std::string endpoint(const std::string &rel) const;
};

} // namespace Slic3r

#endif // slic3r_NonplanarSliceClient_hpp_
