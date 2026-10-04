#ifndef slic3r_DuetRrfClient_hpp_
#define slic3r_DuetRrfClient_hpp_

#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include "DuetClient.hpp"

namespace Slic3r {

class Http;

// DuetClient over the RepRapFirmware "rr_*" HTTP API, as served by a Duet board running in
// standalone mode (no SBC).
//
// Note that the /machine/* REST endpoints are Duet Software Framework (SBC) only and are not
// served by the firmware itself, so nothing here falls back to them the way Duet.cpp does.
class DuetRrfClient : public DuetClient
{
public:
    DuetRrfClient(std::string host, std::string password);

    Capabilities capabilities() const override { return Capabilities{}; }   // HTTP can do everything
    const std::string &endpoint() const override { return m_host; }

    // Establishes a session. RRF binds sessions to the client IP; a session key is requested and
    // used when the board offers one (RRF >= 3.5), but is not required.
    ConnectResult connect(std::string &error_out) override;
    void disconnect() override;

    // GET rr_model?key=<key>&flags=<flags>. Returns the "result" subtree.
    bool object_model(const std::string &key, const std::string &flags, boost::property_tree::ptree &result_out, std::string &error_out) override;

    // GET rr_gcode?gcode=<cmd>. Does not wait for the command to complete.
    bool gcode(const std::string &cmd, std::string &error_out) override;

    // Uploads a binary blob to an absolute SD path (e.g. "0:/firmware/Duet3Firmware_Mini5plus.uf2").
    //
    // The payload is passed as raw bytes and sent verbatim: RRF requires a Content-Length header
    // and rejects the request without one, and the file must not be transformed in any way.
    // A CRC-32 of the payload is always sent so the board validates the transfer itself.
    bool upload(const std::string &remote_path,
                const std::vector<uint8_t> &data,
                ProgressFn progress,
                std::string &error_out) override;

    // GET rr_download?name=<path>. missing_out distinguishes "the board answered, there is no such
    // file" (a normal answer for an optional file) from a transport failure.
    bool download(const std::string &remote_path,
                  std::vector<uint8_t> &out,
                  bool &missing_out,
                  std::string &error_out) override;

    // GET rr_filelist?dir=<dir>, following the "next" cursor until the listing is complete.
    // missing_out is set when the board reports err=2, i.e. the directory does not exist.
    bool filelist(const std::string &dir,
                  std::vector<FileEntry> &out,
                  bool &missing_out,
                  std::string &error_out) override;

    // GET rr_mkdir?dir=<dir>. RRF answers err=1 both for a real failure and for a directory that
    // already exists, so a false return here is advisory: let the upload that follows decide.
    bool mkdir(const std::string &dir, std::string &error_out) override;

    // GET rr_delete?name=<path>.
    bool remove(const std::string &remote_path, std::string &error_out) override;

    // Polls rr_connect until the board answers or the deadline passes. Used after M997, when the
    // board reboots and drops every session. Returns true once reconnected.
    bool wait_for_reboot(int timeout_seconds,
                         const std::function<bool()> &should_cancel,
                         const std::function<void(int elapsed_seconds)> &on_tick,
                         std::string &error_out) override;

    // Kept as an alias for endpoint() so existing call sites read naturally.
    const std::string &host() const { return m_host; }

private:
    std::string m_host;
    std::string m_password;
    // Session key handed out by RRF >= 3.5 when asked; sent back as the X-Session-Key header.
    boost::optional<uint32_t> m_session_key;

    std::string base_url() const;
    std::string timestamp_str() const;
    // Applies the session key header when we have one.
    void apply_session(Http &http) const;
    // "&sessionKey=<n>", or empty. The write endpoints want it in the query string too.
    std::string session_query() const;
};

} // namespace Slic3r

#endif // slic3r_DuetRrfClient_hpp_
