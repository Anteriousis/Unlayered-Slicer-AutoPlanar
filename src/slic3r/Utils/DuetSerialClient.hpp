#ifndef slic3r_DuetSerialClient_hpp_
#define slic3r_DuetSerialClient_hpp_

#include <memory>
#include <string>
#include <vector>

#include "DuetClient.hpp"

// Included rather than forward declared: boost::asio::serial_port is a typedef for
// basic_serial_port<>, not a class, so declaring it as one is a redefinition with a different type
// and every use of it downstream falls over. Serial.hpp pulls in boost/asio wholesale anyway.
#include <boost/asio/io_context.hpp>
#include <boost/asio/serial_port.hpp>

namespace Slic3r {

// DuetClient over the board's USB serial port.
//
// The whole point of this class is that a printer with no network - a machine on the bench, a
// customer with no Ethernet run - can still be updated. Everything it does is the G-code equivalent
// of the rr_* call the HTTP client makes:
//
//     rr_model    ->  M409 K"key" F"flags"
//     rr_upload   ->  M559 P"<absolute path>" S<size> C<crc32>  + exactly <size> raw bytes
//     rr_mkdir    ->  M470 P"dir"          (P, not the D the Duet docs show)
//     rr_filelist ->  M20 S3 P"dir"
//     rr_delete   ->  M30 <path>
//     rr_download ->  M1763 P"path" O<off> L<len>   (this fork only - see capabilities())
//
// There is no password and no session: USB is physical access.
//
// Verified against a Duet 3 Mini 5+ on 2026-09-13, including a 1,926,656-byte firmware image in
// 14.1 s at 134 kB/s with the CRC checked by the board.
class DuetSerialClient : public DuetClient
{
public:
    // port_name is an OS port name ("COM3", "/dev/ttyACM0"). topology_key identifies the physical
    // USB socket, and is what wait_for_reboot() re-finds the board by: a firmware update changes
    // the port's number, its PID and even how many ports the board presents, and RRF reports an
    // empty USB serial number, so nothing else about the device survives.
    DuetSerialClient(std::string port_name, std::string topology_key);
    ~DuetSerialClient() override;

    Capabilities       capabilities() const override { return m_caps; }
    const std::string &endpoint() const override { return m_port_name; }

    ConnectResult connect(std::string &error_out) override;
    void          disconnect() override;

    bool object_model(const std::string &key, const std::string &flags,
                      boost::property_tree::ptree &result_out, std::string &error_out) override;
    bool gcode(const std::string &cmd, std::string &error_out) override;
    bool upload(const std::string &remote_path, const std::vector<uint8_t> &data,
                ProgressFn progress, std::string &error_out) override;
    bool download(const std::string &remote_path, std::vector<uint8_t> &out,
                  bool &missing_out, std::string &error_out) override;
    bool filelist(const std::string &dir, std::vector<FileEntry> &out,
                  bool &missing_out, std::string &error_out) override;
    bool mkdir(const std::string &dir, std::string &error_out) override;
    bool remove(const std::string &remote_path, std::string &error_out) override;
    bool wait_for_reboot(int timeout_seconds,
                         const std::function<bool()> &should_cancel,
                         const std::function<void(int elapsed_seconds)> &on_tick,
                         std::string &error_out) override;

    // CRC-32 of a file already on the card, via M38. The board computes it with the same variant
    // compute_crc32() uses, so an upload can be verified independently of M559's own check.
    bool file_crc32(const std::string &remote_path, uint32_t &crc_out, std::string &error_out);

private:
    std::string  m_port_name;
    std::string  m_topology_key;
    Capabilities m_caps;

    std::unique_ptr<boost::asio::io_context>  m_io;
    std::unique_ptr<boost::asio::serial_port> m_port;
    std::string                               m_rx;      // bytes read but not yet consumed

    bool open_port(const std::string &port_name, std::string &error_out);
    void close_port();

    // Sends one line and collects everything up to its "ok". Both USB channels run in Marlin
    // compatibility, so every command is acknowledged that way; unsolicited output that arrives in
    // between is collected rather than treated as an error, because an RRF channel emits it freely.
    bool transact(const std::string &cmd, std::string &reply_out, int timeout_ms, std::string &error_out);
    bool collect_until_ok(std::string &reply_out, int timeout_ms, const std::string &what, std::string &error_out);
    bool read_some(int timeout_ms, std::string &error_out);
    bool write_all(const uint8_t *data, size_t len, std::string &error_out);
    void discard_input(int quiet_ms);

    // Rejects anything that would break out of a quoted G-code string. A corrupted quoted string
    // wedges RRF's parser, and during an upload that turns megabytes of firmware into commands.
    static bool path_is_safe(const std::string &s);
};

} // namespace Slic3r

#endif // slic3r_DuetSerialClient_hpp_
