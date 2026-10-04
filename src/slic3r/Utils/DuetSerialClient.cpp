#include "DuetSerialClient.hpp"
#include "Serial.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/asio/write.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace asio = boost::asio;
namespace pt   = boost::property_tree;

namespace Slic3r {

namespace {

// Baud is meaningless to a native-USB CDC device - the board ignores it - but a value must be set.
constexpr unsigned SERIAL_BAUD = 115200;

// How long to wait for the "ok" of an ordinary command. M409 and M20 on a busy board are the slow
// ones; M38 hashes the whole file (about 5 s for a 2 MB image) and gets its own budget.
constexpr int TIMEOUT_SHORT_MS  = 5000;
constexpr int TIMEOUT_MODEL_MS  = 10000;
constexpr int TIMEOUT_HASH_MS   = 60000;
// The reply at the end of an upload waits on the final SD flush of a file that may be megabytes.
constexpr int TIMEOUT_UPLOAD_MS = 180000;

// Sent to the port in one go. Large enough that the fixed per-write cost disappears, small enough
// that progress moves visibly on a slow link.
constexpr size_t UPLOAD_CHUNK = 8192;

std::string base64_decode(const std::string &in)
{
    auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc  = 0;
    int      bits = 0;
    for (unsigned char c : in) {
        const int v = value(c);
        if (v < 0)
            continue;               // '=' padding, and any whitespace the reply picked up
        acc  = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// The first {...} of a reply, so a JSON answer can be found among whatever else the channel said.
bool extract_json(const std::string &reply, pt::ptree &out)
{
    const size_t start = reply.find('{');
    if (start == std::string::npos)
        return false;
    try {
        std::istringstream iss(reply.substr(start));
        pt::read_json(iss, out);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace

DuetSerialClient::DuetSerialClient(std::string port_name, std::string topology_key)
    : m_port_name(std::move(port_name)), m_topology_key(std::move(topology_key))
{
    // Settled for real in connect(), once the board has said whether it has M1763.
    m_caps.read_files       = false;
    m_caps.read_files_exact = false;
    m_caps.stable_endpoint  = false;
}

DuetSerialClient::~DuetSerialClient() { close_port(); }

bool DuetSerialClient::path_is_safe(const std::string &s)
{
    return s.find('"') == std::string::npos && s.find('\\') == std::string::npos
        && s.find('\r') == std::string::npos && s.find('\n') == std::string::npos;
}

bool DuetSerialClient::open_port(const std::string &port_name, std::string &error_out)
{
    close_port();
    try {
        m_io   = std::make_unique<asio::io_context>();
        m_port = std::make_unique<asio::serial_port>(*m_io);

#ifdef _WIN32
        // CreateFile does not resolve COM10 and above without the \\.\ prefix, so a board that
        // happens to land on a high port number would simply fail to open.
        const std::string device = boost::starts_with(port_name, "\\\\.\\")
                                       ? port_name : ("\\\\.\\" + port_name);
#else
        const std::string device = port_name;
#endif
        m_port->open(device);
        m_port->set_option(asio::serial_port_base::baud_rate(SERIAL_BAUD));
        m_port->set_option(asio::serial_port_base::character_size(8));
        m_port->set_option(asio::serial_port_base::parity(asio::serial_port_base::parity::none));
        m_port->set_option(asio::serial_port_base::stop_bits(asio::serial_port_base::stop_bits::one));
        m_port->set_option(asio::serial_port_base::flow_control(asio::serial_port_base::flow_control::none));

#ifdef _WIN32
        // DTR must be asserted or RRF ignores this channel completely: it gates on the CDC line
        // state, so the port opens, looks alive, and answers nothing at all. RTS is left alone -
        // it means nothing to a native-USB board, and toggling it resets anything that has an
        // auto-reset circuit.
        ::EscapeCommFunction(m_port->native_handle(), SETDTR);
#endif
    } catch (const std::exception &e) {
        // Worth naming precisely: a Duet whose driver failed to start still enumerates and still
        // appears in the port list, so "cannot open" is a common and confusing state.
        error_out = (boost::format("Could not open %1%: %2%") % port_name % e.what()).str();
        close_port();
        return false;
    }
    m_rx.clear();
    return true;
}

void DuetSerialClient::close_port()
{
    if (m_port) {
        boost::system::error_code ec;
        m_port->close(ec);
        m_port.reset();
    }
    m_io.reset();
    m_rx.clear();
}

bool DuetSerialClient::read_some(int timeout_ms, std::string &error_out)
{
    if (!m_port)
        return false;

    char                      buf[512];
    boost::system::error_code ec;
    size_t                    got = 0;

    m_io->restart();
    m_port->async_read_some(asio::buffer(buf, sizeof(buf)),
                            [&](const boost::system::error_code &e, size_t n) { ec = e; got = n; });
    m_io->run_for(std::chrono::milliseconds(timeout_ms));

    if (got == 0) {
        // Nothing arrived in the window. Cancel the outstanding read so the next call starts clean.
        boost::system::error_code cancel_ec;
        m_port->cancel(cancel_ec);
        m_io->run();
        if (ec && ec != asio::error::operation_aborted) {
            error_out = ec.message();
            return false;
        }
        return true;                        // a quiet window is not an error
    }
    m_rx.append(buf, got);
    return true;
}

bool DuetSerialClient::collect_until_ok(std::string &reply_out, int timeout_ms,
                                        const std::string &what, std::string &error_out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string collected;

    for (;;) {
        size_t nl;
        while ((nl = m_rx.find('\n')) != std::string::npos) {
            std::string line = m_rx.substr(0, nl);
            m_rx.erase(0, nl + 1);
            boost::trim_right_if(line, boost::is_any_of("\r"));

            if (line == "ok" || boost::starts_with(line, "ok ")) {
                // "ok <reply>" is the M105/M998 shape; keep whatever rode along with it.
                const std::string rest = boost::trim_copy(line.substr(2));
                if (!rest.empty()) {
                    if (!collected.empty()) collected += "\n";
                    collected += rest;
                }
                reply_out = collected;
                return true;
            }
            if (!line.empty()) {
                if (!collected.empty()) collected += "\n";
                collected += line;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error_out = (boost::format("The printer did not answer %1% within %2% s")
                         % what % (timeout_ms / 1000)).str();
            if (!collected.empty())
                error_out += (boost::format(" (it said: %1%)") % collected).str();
            return false;
        }
        if (!read_some(200, error_out))
            return false;
    }
}

bool DuetSerialClient::transact(const std::string &cmd, std::string &reply_out, int timeout_ms,
                                std::string &error_out)
{
    if (!m_port) {
        error_out = "The serial port is not open";
        return false;
    }
    if (cmd.find('\n') != std::string::npos || cmd.find('\r') != std::string::npos) {
        error_out = "Refusing to send a command containing a line break";
        return false;
    }

    const std::string line = cmd + "\n";
    if (!write_all(reinterpret_cast<const uint8_t *>(line.data()), line.size(), error_out))
        return false;

    return collect_until_ok(reply_out, timeout_ms, cmd, error_out);
}

bool DuetSerialClient::write_all(const uint8_t *data, size_t len, std::string &error_out)
{
    if (!m_port) {
        error_out = "The serial port is not open";
        return false;
    }
    try {
        asio::write(*m_port, asio::buffer(data, len));
        return true;
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not write to %1%: %2%") % m_port_name % e.what()).str();
        return false;
    }
}

void DuetSerialClient::discard_input(int quiet_ms)
{
    std::string err;
    for (int i = 0; i < 5; ++i) {
        const size_t before = m_rx.size();
        if (!read_some(quiet_ms, err))
            break;
        if (m_rx.size() == before)
            break;
    }
    m_rx.clear();
}

DuetClient::ConnectResult DuetSerialClient::connect(std::string &error_out)
{
    if (!open_port(m_port_name, error_out))
        return ConnectResult::unreachable;

    // Terminate any partial line the firmware may still be holding from an earlier session, then
    // throw away whatever it says about it.
    std::string err;
    const char nl = '\n';
    write_all(reinterpret_cast<const uint8_t *>(&nl), 1, err);
    discard_input(150);

    std::string reply;
    if (!transact("M409 K\"state.status\" F\"f\"", reply, TIMEOUT_SHORT_MS, error_out)) {
        // The likeliest cause by far, and it is invisible from here: an upload that was abandoned
        // mid-payload leaves the channel in binary mode for ever, silently eating everything sent
        // to it. Say so, because "not answering" on its own sends people to the cable.
        error_out += "\n" + std::string(
            "If an update was interrupted, restart the printer and try again.");
        close_port();
        return ConnectResult::unreachable;
    }

    // Does this board have M1763? A board without it can still be updated - it just cannot hand
    // files back, so backups and the installed-config readout are unavailable. "not supported" is
    // the answer from a board that has never met this firmware; a board that has it complains
    // about the missing P parameter instead.
    std::string probe;
    if (transact("M1763", probe, TIMEOUT_SHORT_MS, err)) {
        const std::string lower = boost::to_lower_copy(probe);
        m_caps.read_files       = lower.find("not supported") == std::string::npos;
        m_caps.read_files_exact = m_caps.read_files;
    }
    m_caps.stable_endpoint = false;

    BOOST_LOG_TRIVIAL(info) << "DuetSerial: connected on " << m_port_name
                            << ", file read " << (m_caps.read_files ? "available" : "unavailable");
    return ConnectResult::ok;
}

void DuetSerialClient::disconnect() { close_port(); }

bool DuetSerialClient::object_model(const std::string &key, const std::string &flags,
                                    pt::ptree &result_out, std::string &error_out)
{
    if (!path_is_safe(key) || !path_is_safe(flags)) {
        error_out = "Invalid object model key";
        return false;
    }
    std::string reply;
    if (!transact((boost::format("M409 K\"%1%\" F\"%2%\"") % key % flags).str(), reply,
                  TIMEOUT_MODEL_MS, error_out))
        return false;

    pt::ptree root;
    if (!extract_json(reply, root)) {
        error_out = (boost::format("Could not parse the object model response for %1%") % key).str();
        return false;
    }
    if (auto res = root.get_child_optional("result")) {
        result_out = *res;
        return true;
    }
    // A key the firmware does not know answers with no "result" member at all.
    error_out = (boost::format("The printer did not report %1%") % key).str();
    return false;
}

bool DuetSerialClient::gcode(const std::string &cmd, std::string &error_out)
{
    // M997 and M999 never answer: the firmware shuts the USB interface down as part of doing them.
    // A timeout on those is the expected outcome, not a failure.
    const std::string upper   = boost::to_upper_copy(boost::trim_copy(cmd));
    const bool        reboots = boost::starts_with(upper, "M997") || boost::starts_with(upper, "M999");

    std::string reply;
    if (!transact(cmd, reply, reboots ? 4000 : TIMEOUT_SHORT_MS, error_out)) {
        if (reboots) {
            error_out.clear();
            return true;
        }
        return false;
    }
    if (reply.find("Error:") != std::string::npos) {
        error_out = reply;
        return false;
    }
    return true;
}

bool DuetSerialClient::upload(const std::string &remote_path, const std::vector<uint8_t> &data,
                              ProgressFn progress, std::string &error_out)
{
    if (!path_is_safe(remote_path)) {
        error_out = (boost::format("Invalid path: %1%") % remote_path).str();
        return false;
    }

    const uint32_t crc = compute_crc32(data.data(), data.size());

    // Open the file first and WAIT for the board to confirm it. This ordering is the single most
    // important rule in this class: if the open failed - a full card, a missing directory, a line
    // corrupted in transit - the channel is still parsing G-code, and a firmware image sent into it
    // would be executed as commands on a machine with heaters and motors.
    std::string reply;
    if (!transact((boost::format("M559 P\"%1%\" S%2% C%3%") % remote_path % data.size() % crc).str(),
                  reply, TIMEOUT_MODEL_MS, error_out))
        return false;
    if (reply.find("Writing to file") == std::string::npos) {
        error_out = reply.empty()
            ? (boost::format("The printer would not open %1% for writing") % remote_path).str()
            : reply;
        return false;
    }

    bool   cancel = false;
    size_t sent   = 0;
    while (sent < data.size()) {
        const size_t n = std::min(UPLOAD_CHUNK, data.size() - sent);
        if (!write_all(data.data() + sent, n, error_out)) {
            // The board is still counting down to the size it was promised, and there is no way to
            // abort - so leave nothing behind that a later M997 could pick up.
            std::string ignored;
            remove(remote_path, ignored);
            return false;
        }
        sent += n;
        if (progress)
            progress(sent, data.size(), cancel);
        // Cancelling cannot mean "stop writing": the firmware would sit in binary mode for ever and
        // eat whatever the next session sends. Finish the byte count with padding instead, let the
        // CRC check fail, and delete the file below.
        if (cancel && sent < data.size()) {
            const std::vector<uint8_t> pad(std::min(UPLOAD_CHUNK, data.size() - sent), 0);
            while (sent < data.size()) {
                const size_t m = std::min(pad.size(), data.size() - sent);
                if (!write_all(pad.data(), m, error_out))
                    break;
                sent += m;
            }
            break;
        }
    }

    // One trailing newline, always. Without it roughly 40% of uploads never complete: the final
    // short USB packet is not visible to the firmware until more traffic arrives on the endpoint,
    // so the board sits in binary mode for ever and the next thing sent is eaten as file data.
    // Measured on a Mini 5+ on 2026-09-13: 8 stalls in 20 uploads without, 0 in 30 with.
    //
    // It cannot corrupt anything. If the payload was complete, the byte count is already reached
    // and the newline stays in the parser's ring, where a blank line produces no command and no
    // reply. If a byte ever genuinely were lost, the newline lands in the file instead and the
    // board's own CRC check fails - a loud answer rather than a hang.
    const char nl = '\n';
    if (!write_all(reinterpret_cast<const uint8_t *>(&nl), 1, error_out))
        return false;

    if (!collect_until_ok(reply, TIMEOUT_UPLOAD_MS, "the end of the upload", error_out))
        return false;

    if (cancel) {
        std::string ignored;
        remove(remote_path, ignored);
        error_out = "Cancelled";
        return false;
    }
    if (reply.find("CRC32 checksum doesn't match") != std::string::npos) {
        std::string ignored;
        remove(remote_path, ignored);
        error_out = (boost::format("%1% did not arrive intact and has been removed") % remote_path).str();
        return false;
    }
    if (reply.find("Done saving file") == std::string::npos) {
        error_out = (boost::format("Unexpected reply after writing %1%: %2%") % remote_path % reply).str();
        return false;
    }
    return true;
}

bool DuetSerialClient::file_crc32(const std::string &remote_path, uint32_t &crc_out, std::string &error_out)
{
    if (!path_is_safe(remote_path)) {
        error_out = (boost::format("Invalid path: %1%") % remote_path).str();
        return false;
    }
    // M38 takes an unprecedented string - no P, no quotes. Despite its name and the Duet
    // documentation calling it SHA1, it answers with a CRC-32 as eight lowercase hex digits.
    std::string reply;
    if (!transact("M38 " + remote_path, reply, TIMEOUT_HASH_MS, error_out))
        return false;
    if (reply.find("Cannot open file") != std::string::npos || reply.find("Error:") != std::string::npos) {
        error_out = reply;
        return false;
    }
    const std::string trimmed = boost::trim_copy(reply);
    if (trimmed.size() < 8) {
        error_out = (boost::format("Unexpected checksum reply for %1%: %2%") % remote_path % reply).str();
        return false;
    }
    try {
        crc_out = static_cast<uint32_t>(std::stoul(trimmed.substr(trimmed.size() - 8), nullptr, 16));
    } catch (const std::exception &) {
        error_out = (boost::format("Unexpected checksum reply for %1%: %2%") % remote_path % reply).str();
        return false;
    }
    return true;
}

bool DuetSerialClient::download(const std::string &remote_path, std::vector<uint8_t> &out,
                                bool &missing_out, std::string &error_out)
{
    missing_out = false;
    out.clear();
    if (!path_is_safe(remote_path)) {
        error_out = (boost::format("Invalid path: %1%") % remote_path).str();
        return false;
    }
    if (!m_caps.read_files) {
        error_out = "This printer's firmware cannot send files back over USB; update it over the "
                    "network, or update its firmware first.";
        return false;
    }

    // M1763 pages through the file; each reply carries what was actually read, so continue from
    // offset+len rather than assuming the chunk size asked for.
    uint64_t offset = 0;
    for (;;) {
        std::string reply;
        if (!transact((boost::format("M1763 P\"%1%\" O%2% L512") % remote_path % offset).str(),
                      reply, TIMEOUT_MODEL_MS, error_out))
            return false;

        if (reply.find("Cannot open file") != std::string::npos) {
            missing_out = true;                 // a normal answer for an optional file
            return false;
        }
        pt::ptree root;
        if (!extract_json(reply, root)) {
            error_out = (boost::format("Could not parse the file read response for %1%: %2%")
                         % remote_path % reply).str();
            return false;
        }
        const auto node = root.get_child_optional("fread");
        if (!node) {
            error_out = (boost::format("Unexpected file read response for %1%") % remote_path).str();
            return false;
        }
        const std::string decoded = base64_decode(node->get<std::string>("data", ""));
        out.insert(out.end(), decoded.begin(), decoded.end());

        const uint64_t len = node->get<uint64_t>("len", 0);
        if (node->get<bool>("eof", true) || len == 0)
            return true;
        offset += len;
    }
}

bool DuetSerialClient::filelist(const std::string &dir, std::vector<FileEntry> &out,
                                bool &missing_out, std::string &error_out)
{
    missing_out = false;
    out.clear();
    if (!path_is_safe(dir)) {
        error_out = (boost::format("Invalid path: %1%") % dir).str();
        return false;
    }

    int64_t first = 0;
    for (;;) {
        std::string reply;
        if (!transact((boost::format("M20 S3 P\"%1%\" R%2%") % dir % first).str(), reply,
                      TIMEOUT_MODEL_MS, error_out))
            return false;

        pt::ptree root;
        if (!extract_json(reply, root)) {
            error_out = (boost::format("Could not parse the listing for %1%") % dir).str();
            return false;
        }
        const int err = root.get<int>("err", 0);
        if (err == 2) {
            missing_out = true;                 // the directory does not exist - a normal answer
            return false;
        }
        if (err != 0) {
            error_out = (boost::format("The printer could not list %1% (err %2%)") % dir % err).str();
            return false;
        }
        if (auto files = root.get_child_optional("files")) {
            for (const auto &kv : *files) {
                FileEntry e;
                const std::string type = kv.second.get<std::string>("type", "f");
                e.is_dir = (type == "d");
                e.name   = kv.second.get<std::string>("name", "");
                e.size   = kv.second.get<uint64_t>("size", 0);
                e.date   = kv.second.get<std::string>("date", "");
                if (!e.name.empty())
                    out.push_back(std::move(e));
            }
        }
        const int64_t next = root.get<int64_t>("next", 0);
        if (next <= 0)
            return true;
        first = next;
    }
}

bool DuetSerialClient::mkdir(const std::string &dir, std::string &error_out)
{
    if (!path_is_safe(dir)) {
        error_out = (boost::format("Invalid path: %1%") % dir).str();
        return false;
    }
    // M470 takes P, not the D the Duet documentation shows - confirmed against a live board.
    // Like rr_mkdir, a failure here cannot be told apart from "it already exists", so the result is
    // advisory and the upload that follows is the real test.
    std::string reply;
    if (!transact((boost::format("M470 P\"%1%\"") % dir).str(), reply, TIMEOUT_SHORT_MS, error_out))
        return false;
    if (reply.find("Error:") != std::string::npos) {
        error_out = reply;
        return false;
    }
    return true;
}

bool DuetSerialClient::remove(const std::string &remote_path, std::string &error_out)
{
    if (!path_is_safe(remote_path)) {
        error_out = (boost::format("Invalid path: %1%") % remote_path).str();
        return false;
    }
    std::string reply;
    if (!transact("M30 " + remote_path, reply, TIMEOUT_SHORT_MS, error_out))
        return false;
    if (reply.find("Error:") != std::string::npos) {
        error_out = reply;
        return false;
    }
    return true;
}

bool DuetSerialClient::wait_for_reboot(int timeout_seconds,
                                       const std::function<bool()> &should_cancel,
                                       const std::function<void(int)> &on_tick,
                                       std::string &error_out)
{
    // The board is about to vanish from the bus, so let go of the handle before it does.
    close_port();

    // The same grace the HTTP client takes, for the same reason: the old firmware is still running
    // for a moment and would answer.
    std::this_thread::sleep_for(std::chrono::seconds(4));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    const auto started  = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        if (should_cancel && should_cancel()) {
            error_out = "Cancelled";
            return false;
        }
        if (on_tick)
            on_tick(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count()));

        // Re-find the board by the USB socket it is plugged into, not by its port name. A firmware
        // update can change the port number, the USB product id, and how many ports the board
        // presents at all (RRF 3.6 offers one, 3.7 offers two) - and the board reports an empty USB
        // serial number, so the physical location is the only thing that survives.
        for (const Utils::SerialPortInfo &p : Utils::scan_duet_ports()) {
            if (!m_topology_key.empty() && p.topology_key != m_topology_key)
                continue;
            std::string err;
            if (!open_port(p.port, err))
                continue;
            std::string reply;
            if (transact("M409 K\"state.status\" F\"f\"", reply, 3000, err)) {
                m_port_name = p.port;           // it may well have moved
                BOOST_LOG_TRIVIAL(info) << "DuetSerial: board came back on " << m_port_name;
                return true;
            }
            close_port();                       // enumerated but not yet servicing USB
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    error_out = "The printer did not reappear on USB after restarting";
    return false;
}

} // namespace Slic3r
