#include "DuetRrfClient.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/miniz_extension.hpp"
#include "Http.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

// Connect/model/gcode calls are small and should fail fast; uploads get their own generous
// timeout because a firmware image is ~2 MB over WiFi.
static const long RRF_TIMEOUT_CONNECT  = 5;
static const long RRF_TIMEOUT_SHORT    = 10;
static const long RRF_TIMEOUT_UPLOAD   = 300;

DuetRrfClient::DuetRrfClient(std::string host, std::string password)
    : m_host(std::move(host)), m_password(std::move(password))
{}

std::string DuetRrfClient::base_url() const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0)
        return m_host.back() == '/' ? m_host : (boost::format("%1%/") % m_host).str();
    return (boost::format("http://%1%/") % m_host).str();
}

std::string DuetRrfClient::timestamp_str() const
{
    enum { BUFFER_SIZE = 32 };
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buffer[BUFFER_SIZE];
    std::strftime(buffer, BUFFER_SIZE, "time=%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buffer);
}

void DuetRrfClient::apply_session(Http &http) const
{
    if (m_session_key)
        http.header("X-Session-Key", std::to_string(*m_session_key));
}

std::string DuetRrfClient::session_query() const
{
    // The write endpoints want the session key in the query string as well as in the header;
    // sending only the header has been seen to come back as "Not authorized" on 3.7.
    return m_session_key ? (boost::format("&sessionKey=%1%") % *m_session_key).str() : std::string();
}

DuetRrfClient::ConnectResult DuetRrfClient::connect(std::string &error_out)
{
    m_session_key = boost::none;

    // sessionKey=yes is honoured by RRF >= 3.5 and ignored by older builds, which fall back to
    // binding the session to our IP address.
    auto url = (boost::format("%1%rr_connect?password=%2%&sessionKey=yes&%3%")
                % base_url()
                % Http::url_encode(m_password.empty() ? "reprap" : m_password)
                % timestamp_str()).str();

    auto result = ConnectResult::unreachable;
    auto http   = Http::get(std::move(url));
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: connect failed: %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
            result    = ConnectResult::unreachable;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                const int err = root.get<int>("err", 0);
                switch (err) {
                case 0:
                    result = ConnectResult::ok;
                    // Only present from RRF 3.5 onwards.
                    if (auto key = root.get_optional<uint32_t>("sessionKey"))
                        m_session_key = *key;
                    break;
                case 1: result = ConnectResult::wrong_password;  error_out = "Wrong password"; break;
                case 2: result = ConnectResult::no_free_session; error_out = "The board has no free connection slots"; break;
                default:
                    result    = ConnectResult::unreachable;
                    error_out = (boost::format("Unexpected error code %1% from rr_connect") % err).str();
                    break;
                }
            } catch (const std::exception &e) {
                result    = ConnectResult::unreachable;
                error_out = (boost::format("Could not parse rr_connect response: %1%") % e.what()).str();
            }
        })
        .perform_sync();

    return result;
}

void DuetRrfClient::disconnect()
{
    auto url  = (boost::format("%1%rr_disconnect") % base_url()).str();
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([](std::string, std::string error, unsigned) {
            // Not worth surfacing: RRF times the session out on its own.
            BOOST_LOG_TRIVIAL(debug) << boost::format("DuetRrf: disconnect failed (ignored): %1%") % error;
        })
        .perform_sync();
    m_session_key = boost::none;
}

bool DuetRrfClient::object_model(const std::string &key, const std::string &flags, pt::ptree &result_out, std::string &error_out)
{
    auto url = (boost::format("%1%rr_model?key=%2%&flags=%3%")
                % base_url()
                % Http::url_encode(key)
                % Http::url_encode(flags)).str();

    bool ok   = false;
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: rr_model(%1%) failed: %2%, HTTP %3%") % key % error % status;
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                if (auto res = root.get_child_optional("result")) {
                    result_out = *res;
                    ok         = true;
                } else {
                    error_out = (boost::format("rr_model(%1%) returned no result") % key).str();
                }
            } catch (const std::exception &e) {
                error_out = (boost::format("Could not parse rr_model(%1%) response: %2%") % key % e.what()).str();
            }
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::gcode(const std::string &cmd, std::string &error_out)
{
    auto url = (boost::format("%1%rr_gcode?gcode=%2%") % base_url() % Http::url_encode(cmd)).str();

    bool ok   = false;
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: rr_gcode(%1%) failed: %2%, HTTP %3%") % cmd % error % status;
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            // A successful queue returns {"buff":<free space>}; {"err":1} means the input buffer
            // was full and the command was dropped on the floor.
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                if (root.get<int>("err", 0) != 0)
                    error_out = (boost::format("The board rejected the command %1%") % cmd).str();
                else
                    ok = true;
            } catch (const std::exception &) {
                // Older firmware answers with a bare body; treat a 2xx as success.
                ok = true;
            }
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::upload(const std::string &remote_path,
                           const std::vector<uint8_t> &data,
                           ProgressFn progress,
                           std::string &error_out)
{
    if (data.empty()) {
        error_out = (boost::format("Refusing to upload an empty file to %1%") % remote_path).str();
        return false;
    }

    const uint32_t crc = compute_crc32(data.data(), data.size());

    auto url = (boost::format("%1%rr_upload?name=%2%&%3%&crc32=%4$08x")
                % base_url()
                % Http::url_encode(remote_path)
                % timestamp_str()
                % crc).str();

    BOOST_LOG_TRIVIAL(info) << boost::format("DuetRrf: uploading %1% bytes to %2% (crc32=%3$08x)")
                                   % data.size() % remote_path % crc;

    // The payload must reach the board byte-for-byte. Http::set_post_body(path) reads the file
    // through a text-mode ifstream, which on Windows collapses CRLF pairs and stops at 0x1A, so
    // it cannot be used for binary images. The std::string overload sets CURLOPT_POSTFIELDS with
    // an explicit CURLOPT_POSTFIELDSIZE_LARGE, which is both binary-safe (embedded NULs included)
    // and supplies the Content-Length header that RRF requires for rr_upload.
    std::string body(reinterpret_cast<const char *>(data.data()), data.size());

    bool ok        = false;
    bool cancelled = false;
    auto http      = Http::post(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_UPLOAD)
        .header("Content-Type", "application/octet-stream")
        .set_post_body(body)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: upload of %1% failed: %2%, HTTP %3%, body: `%4%`")
                                            % remote_path % error % status % body;
            if (!cancelled)
                error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                const int err = root.get<int>("err", 0);
                if (err != 0) {
                    // RRF reports err=1 for a write failure, a length mismatch or a CRC mismatch.
                    error_out = (boost::format("The board rejected the upload of %1% (error %2%); "
                                               "this usually means a corrupted transfer or a full SD card")
                                 % remote_path % err).str();
                } else {
                    ok = true;
                }
            } catch (const std::exception &e) {
                error_out = (boost::format("Could not parse the upload response for %1%: %2%") % remote_path % e.what()).str();
            }
        })
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress)
                progress(p.ulnow, p.ultotal ? p.ultotal : data.size(), cancel);
            if (cancel) {
                cancelled = true;
                error_out = "Cancelled";
            }
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::download(const std::string &remote_path,
                             std::vector<uint8_t> &out,
                             bool &missing_out,
                             std::string &error_out)
{
    missing_out = false;
    out.clear();

    auto url = (boost::format("%1%rr_download?name=%2%") % base_url() % Http::url_encode(remote_path)).str();

    bool ok   = false;
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_UPLOAD)
        .on_error([&](std::string body, std::string error, unsigned status) {
            // 404 is the board telling us the file is not there, which several callers treat as a
            // perfectly ordinary answer rather than an error.
            if (status == 404) {
                missing_out = true;
                return;
            }
            BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: rr_download(%1%) failed: %2%, HTTP %3%")
                                            % remote_path % error % status;
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            // The body is the raw file; std::string carries embedded NULs, so this is binary safe.
            out.assign(body.begin(), body.end());
            ok = true;
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::filelist(const std::string &dir,
                             std::vector<FileEntry> &out,
                             bool &missing_out,
                             std::string &error_out)
{
    missing_out = false;
    out.clear();

    // RRF returns as much as fits in its buffers and hands back a cursor for the rest.
    unsigned int first = 0;
    for (int page = 0; page < 64; ++page) {
        auto url = (boost::format("%1%rr_filelist?dir=%2%&first=%3%")
                    % base_url() % Http::url_encode(dir) % first).str();

        bool         ok   = false;
        unsigned int next = 0;
        auto         http = Http::get(std::move(url));
        apply_session(http);
        http.timeout_connect(RRF_TIMEOUT_CONNECT)
            .timeout_max(RRF_TIMEOUT_SHORT)
            .on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("DuetRrf: rr_filelist(%1%) failed: %2%, HTTP %3%")
                                                % dir % error % status;
                error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
            })
            .on_complete([&](std::string body, unsigned) {
                try {
                    pt::ptree root;
                    std::istringstream iss(body);
                    pt::read_json(iss, root);

                    const int err = root.get<int>("err", 0);
                    if (err == 2) {
                        // No such directory. Not a failure: the caller decides what that means.
                        missing_out = true;
                        ok          = true;
                        return;
                    }
                    if (err != 0) {
                        error_out = (boost::format("The board could not list %1% (error %2%)") % dir % err).str();
                        return;
                    }

                    if (auto files = root.get_child_optional("files")) {
                        for (const auto &kv : *files) {
                            FileEntry e;
                            e.name   = kv.second.get<std::string>("name", "");
                            e.size   = kv.second.get<uint64_t>("size", 0);
                            e.date   = kv.second.get<std::string>("date", "");
                            e.is_dir = kv.second.get<std::string>("type", "f") == "d";
                            if (!e.name.empty())
                                out.push_back(std::move(e));
                        }
                    }
                    next = root.get<unsigned int>("next", 0);
                    ok   = true;
                } catch (const std::exception &e) {
                    error_out = (boost::format("Could not parse the file list for %1%: %2%") % dir % e.what()).str();
                }
            })
            .perform_sync();

        if (!ok)
            return false;
        if (missing_out || next == 0)
            return true;
        first = next;
    }

    error_out = (boost::format("The listing of %1% did not end") % dir).str();
    return false;
}

bool DuetRrfClient::mkdir(const std::string &dir, std::string &error_out)
{
    auto url = (boost::format("%1%rr_mkdir?dir=%2%%3%")
                % base_url() % Http::url_encode(dir) % session_query()).str();

    bool ok   = false;
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([&](std::string body, std::string error, unsigned status) {
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                if (root.get<int>("err", 0) == 0)
                    ok = true;
                else
                    error_out = (boost::format("The board did not create %1%") % dir).str();
            } catch (const std::exception &e) {
                error_out = (boost::format("Could not parse the mkdir response for %1%: %2%") % dir % e.what()).str();
            }
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::remove(const std::string &remote_path, std::string &error_out)
{
    auto url = (boost::format("%1%rr_delete?name=%2%%3%")
                % base_url() % Http::url_encode(remote_path) % session_query()).str();

    bool ok   = false;
    auto http = Http::get(std::move(url));
    apply_session(http);
    http.timeout_connect(RRF_TIMEOUT_CONNECT)
        .timeout_max(RRF_TIMEOUT_SHORT)
        .on_error([&](std::string body, std::string error, unsigned status) {
            error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                pt::ptree root;
                std::istringstream iss(body);
                pt::read_json(iss, root);
                if (root.get<int>("err", 0) == 0)
                    ok = true;
                else
                    error_out = (boost::format("The board did not delete %1%") % remote_path).str();
            } catch (const std::exception &e) {
                error_out = (boost::format("Could not parse the delete response for %1%: %2%") % remote_path % e.what()).str();
            }
        })
        .perform_sync();

    return ok;
}

bool DuetRrfClient::wait_for_reboot(int timeout_seconds,
                                    const std::function<bool()> &should_cancel,
                                    const std::function<void(int)> &on_tick,
                                    std::string &error_out)
{
    using clock = std::chrono::steady_clock;
    const auto start    = clock::now();
    const auto deadline = start + std::chrono::seconds(timeout_seconds);

    // Give the board a moment to actually tear the network stack down; polling immediately would
    // otherwise succeed against the still-running old firmware and we'd declare victory early.
    std::this_thread::sleep_for(std::chrono::seconds(4));

    while (clock::now() < deadline) {
        if (should_cancel && should_cancel()) {
            error_out = "Cancelled";
            return false;
        }

        std::string ignored;
        if (connect(ignored) == ConnectResult::ok) {
            // The board is up, but may still report "updating" while a secondary module is
            // flashed. Let the caller decide what to do with that; being reachable is enough.
            return true;
        }

        if (on_tick)
            on_tick(static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count()));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    error_out = (boost::format("The printer did not come back within %1% seconds") % timeout_seconds).str();
    return false;
}

} // namespace Slic3r
