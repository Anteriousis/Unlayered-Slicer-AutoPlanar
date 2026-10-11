#include "UsageTelemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "Http.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r {
namespace UsageTelemetry {

namespace {

// Undocumented override, like "version_check_url": "usage_diagnostics_url" in the app config,
// for pointing a test copy at a local site.
const char *DEFAULT_URL = "https://unlayered3d.com/api/slicer/telemetry";

constexpr size_t FLUSH_AT_EVENTS      = 50;
constexpr size_t MAX_EVENTS_PER_BATCH = 100; // the site rejects more
constexpr size_t MAX_QUEUED           = 400; // offline for a long session: drop, never grow
constexpr size_t MAX_PENDING_BATCHES  = 20;  // unsent batches kept on disk between launches
constexpr double EXIT_BUDGET_S        = 6.0; // the longest shutdown() may hold up closing the app

struct State
{
    std::mutex        mutex;
    bool              started{false};
    std::atomic<bool> on{false};
    bool              env_off{false};
    bool              first_run{false};

    std::string url, install, session, version, channel, os;
    std::chrono::steady_clock::time_point started_at;
    int64_t                               started_ms{0};

    std::vector<json>          queue;
    std::map<std::string, int> counts;
    std::vector<std::string>   pending;   // batch bodies a failed send left for later
    std::map<int, std::string> in_flight; // bodies being sent in the background
    int                        next_id{0};
};

State &state()
{
    static State s;
    return s;
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string new_uuid() { return boost::uuids::to_string(boost::uuids::random_generator()()); }

bool is_uuid(const std::string &s)
{
    static const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return std::regex_match(s, re);
}

fs::path usage_dir() { return fs::path(data_dir()) / "usage"; }
fs::path pending_path() { return usage_dir() / "pending.json"; }
fs::path marker_path(const std::string &session) { return usage_dir() / ("session-" + session + ".json"); }

std::string os_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    return "linux";
#endif
}

// The site groups by channel so a developer's own copy never reads as a customer.
std::string channel_of(std::string version)
{
    std::transform(version.begin(), version.end(), version.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (version.find("dev") != std::string::npos)
        return "dev";
    for (const char *pre : {"alpha", "beta", "rc"})
        if (version.find(pre) != std::string::npos)
            return "beta";
    return "release";
}

long current_pid()
{
#ifdef _WIN32
    return long(::GetCurrentProcessId());
#else
    return long(::getpid());
#endif
}

bool process_alive(long pid)
{
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        // Access denied means it exists (another user's process); anything else means it is gone.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code  = 0;
    bool  alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string read_file(const fs::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path &path, const std::string &content)
{
    boost::system::error_code ec;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out << content;
        if (!out)
            return;
    }
    fs::rename(tmp, path, ec);
}

// This session's crash marker; see start(). Caller holds the mutex.
void write_marker_locked(const State &s)
{
    write_file(marker_path(s.session),
               json{{"pid", current_pid()}, {"version", s.version}, {"started", s.started_ms}, {"alive", now_ms()}}.dump());
}

json make_event(const std::string &name, json props)
{
    if (!props.is_object())
        props = json::object();
    return json{{"name", name}, {"t", now_ms()}, {"props", std::move(props)}};
}

std::string batch_body(const State &s, std::vector<json> events)
{
    json body = {{"v", 1},
                 {"install", s.install},
                 {"session", s.session},
                 {"app", {{"version", s.version}, {"channel", s.channel}, {"os", s.os}}},
                 {"events", std::move(events)}};
    return body.dump();
}

// Moves up to one batch's worth of queued events into a body. Caller holds the mutex.
std::string take_batch_locked(State &s)
{
    size_t n = std::min(s.queue.size(), MAX_EVENTS_PER_BATCH);
    std::vector<json> events(std::make_move_iterator(s.queue.begin()), std::make_move_iterator(s.queue.begin() + n));
    s.queue.erase(s.queue.begin(), s.queue.begin() + n);
    return batch_body(s, std::move(events));
}

void save_pending_locked(State &s)
{
    boost::system::error_code ec;
    if (s.pending.size() > MAX_PENDING_BATCHES)
        s.pending.erase(s.pending.begin(), s.pending.end() - MAX_PENDING_BATCHES);
    if (s.pending.empty()) {
        fs::remove(pending_path(), ec);
        return;
    }
    write_file(pending_path(), json(s.pending).dump());
}

void finish(int id, bool keep)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.in_flight.find(id);
    if (it == s.in_flight.end())
        return;
    if (keep && s.on) {
        s.pending.push_back(std::move(it->second));
        save_pending_locked(s);
    }
    s.in_flight.erase(it);
}

// Returns whether it was delivered (meaningful for a synchronous send only).
bool send(const std::string &body, bool sync)
{
    State &s = state();
    int id;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        id = s.next_id++;
        s.in_flight[id] = body;
        url = s.url;
    }
    auto delivered = std::make_shared<std::atomic<bool>>(false);
    auto http = Http::post(url);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .timeout_connect(sync ? 3 : 10)
        .timeout_max(sync ? 5 : 30)
        .tls_verify(true)
        .on_complete([id, delivered](std::string, unsigned) {
            delivered->store(true);
            finish(id, false);
        })
        .on_error([id](std::string, std::string error, unsigned status) {
            // Offline, rate-limited or the site down: keep it for later. Any other 4xx means the
            // site will never take this batch, so retrying would only repeat the refusal.
            bool keep = status == 0 || status == 429 || status >= 500;
            BOOST_LOG_TRIVIAL(info) << "usage diagnostics: send failed, status " << status << ", " << error
                                    << (keep ? " (kept for later)" : " (dropped)");
            finish(id, keep);
        });
    if (sync)
        http.perform_sync();
    else
        http.perform();
    return delivered->load();
}

} // namespace

void start(AppConfig &config, bool first_run)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.started)
        return;
    // Whatever an earlier start()/shutdown() pair left is on disk now, and is read back below.
    s.queue.clear();
    s.counts.clear();
    s.pending.clear();

    const char *env = std::getenv("UNLAYERED_NO_TELEMETRY");
    s.env_off    = env && *env;
    s.first_run  = first_run;
    s.version    = UNLAYERED_VERSION;
    s.channel    = channel_of(s.version);
    s.os         = os_name();
    s.started_at = std::chrono::steady_clock::now();
    s.session    = new_uuid();

    std::string url = config.get("usage_diagnostics_url");
    s.url = url.empty() ? DEFAULT_URL : url;

    s.install = config.get("usage_diagnostics_id");
    if (!is_uuid(s.install)) {
        s.install = new_uuid();
        config.set("usage_diagnostics_id", s.install);
    }
    s.on = !s.env_off && config.get_bool("usage_diagnostics");

    boost::system::error_code ec;
    fs::create_directories(usage_dir(), ec);

    // A marker left by a process that no longer runs is a session that never reached shutdown().
    // A marker whose process still runs is a second copy of the app, open right now.
    std::vector<fs::path> markers; // collected first: they are deleted below
    for (fs::directory_iterator it(usage_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string file = it->path().filename().string();
        if (file.rfind("session-", 0) == 0 && it->path().extension() == ".json")
            markers.push_back(it->path());
    }
    for (const fs::path &marker_file : markers) {
        json marker = json::parse(read_file(marker_file), nullptr, false);
        long pid    = marker.is_object() ? marker.value("pid", 0L) : 0L;
        if (pid != 0 && pid != current_pid() && process_alive(pid))
            continue;
        if (s.on && marker.is_object()) {
            json props = {{"version", marker.value("version", std::string())}};
            // "alive" is refreshed by every flush, so this is the uptime to within a few minutes.
            int64_t started = marker.value("started", int64_t(0)), alive = marker.value("alive", started);
            if (started > 0 && alive >= started)
                props["uptime_s"] = (alive - started) / 1000;
            s.queue.push_back(make_event("crash", std::move(props)));
        }
        boost::system::error_code rm_ec;
        fs::remove(marker_file, rm_ec);
    }
    s.started_ms = now_ms();
    write_marker_locked(s);

    if (s.on) {
        json saved = json::parse(read_file(pending_path()), nullptr, false);
        if (saved.is_array())
            for (const auto &b : saved)
                if (b.is_string())
                    s.pending.push_back(b.get<std::string>());
    } else
        fs::remove(pending_path(), ec);

    s.started = true;
    BOOST_LOG_TRIVIAL(info) << "usage diagnostics: " << (s.on ? "on" : (s.env_off ? "off (UNLAYERED_NO_TELEMETRY)" : "off"))
                            << ", channel " << s.channel;
}

bool enabled() { return state().on; }

bool disabled_by_environment() { return state().env_off; }

bool is_first_run() { return state().first_run; }

void set_enabled(AppConfig &config, bool on)
{
    State &s = state();
    config.set_bool("usage_diagnostics", on);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.on = on && !s.env_off;
    if (!s.on) {
        s.queue.clear();
        s.counts.clear();
        s.pending.clear();
        save_pending_locked(s);
    }
    BOOST_LOG_TRIVIAL(info) << "usage diagnostics switched " << (s.on ? "on" : "off");
}

void record(const std::string &name, json props, json counts)
{
    State &s = state();
    if (!s.on)
        return;
    json event = make_event(name, std::move(props));
    if (counts.is_object() && !counts.empty())
        event["counts"] = std::move(counts);
    bool full;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started || s.queue.size() >= MAX_QUEUED)
            return;
        s.queue.push_back(std::move(event));
        full = s.queue.size() >= FLUSH_AT_EVENTS;
    }
    if (full)
        flush();
}

void count(const std::string &feature, int by)
{
    State &s = state();
    if (!s.on || feature.empty())
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    // The site takes at most 200 tallies per event; past that only known ones keep counting.
    if (s.started && (s.counts.size() < 200 || s.counts.count(feature)))
        s.counts[feature] += by;
}

void flush()
{
    State &s = state();
    if (!s.on)
        return;
    std::vector<std::string> bodies;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started)
            return;
        write_marker_locked(s);
        bodies.swap(s.pending);
        if (!bodies.empty())
            save_pending_locked(s);
        while (!s.queue.empty())
            bodies.push_back(take_batch_locked(s));
    }
    for (const std::string &body : bodies)
        send(body, false);
}

void shutdown()
{
    State &s = state();
    std::vector<std::string> bodies;
    std::string session;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started)
            return;
        s.started = false; // nothing queued after this point
        session   = s.session;
        if (s.on) {
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started_at).count();
            json   end     = make_event("session_end", {{"duration_s", int64_t(seconds)}});
            json   counts  = json::object();
            for (const auto &[feature, n] : s.counts)
                counts[feature] = n;
            end["counts"] = std::move(counts);
            s.queue.push_back(std::move(end));
            while (!s.queue.empty())
                bodies.push_back(take_batch_locked(s));
            // This session's newest batch first: if time runs out, older ones wait for next launch.
            std::reverse(bodies.begin(), bodies.end());
            bodies.insert(bodies.end(), s.pending.begin(), s.pending.end());
            s.pending.clear();
            // Background sends that have not answered yet would be lost with the process.
            for (auto &[id, body] : s.in_flight)
                bodies.push_back(body);
            s.in_flight.clear();
        }
    }

    if (!bodies.empty()) {
        auto   began = std::chrono::steady_clock::now();
        size_t next  = 0;
        while (next < bodies.size()) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() > EXIT_BUDGET_S)
                break;
            // A failed send has already been kept or dropped by finish(). The rest would most
            // likely fail the same way (offline), so they wait for the next launch untried.
            if (!send(bodies[next++], true))
                break;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending.insert(s.pending.end(), bodies.begin() + next, bodies.end());
        save_pending_locked(s);
    }

    boost::system::error_code ec;
    fs::remove(marker_path(session), ec);
}

std::string preset_base_name(const PresetCollection &collection, const Preset &preset)
{
    if (preset.is_system)
        return preset.name;
    const Preset *parent = collection.get_preset_parent(preset);
    return parent && parent->is_system ? parent->name : "custom";
}

std::string redact_message(const std::string &message, const std::vector<std::string> &names)
{
    std::string out = message.substr(0, message.find('\n'));

    // Longest names first, so "Bracket" does not leave "_v2" behind from "Bracket_v2".
    std::vector<std::string> sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (const std::string &name : sorted) {
        if (name.size() < 2)
            continue;
        for (size_t at = out.find(name); at != std::string::npos; at = out.find(name, at + 6))
            out.replace(at, name.size(), "<name>");
    }

    static const std::regex quoted(R"("[^"]*"|'[^']*'|\xE2\x80\x9C.*?\xE2\x80\x9D|\xC2\xAB.*?\xC2\xBB)");
    out = std::regex_replace(out, quoted, "<...>");
    static const std::regex path(R"([A-Za-z]:[\\/]\S*|\\\\\S+|(^|\s)/(?:[^\s/]+/)+\S*)");
    out = std::regex_replace(out, path, "$1<path>");

    // Trim, then cut on a UTF-8 character boundary.
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), not_space));
    out.erase(std::find_if(out.rbegin(), out.rend(), not_space).base(), out.end());
    if (out.size() > 160) {
        size_t cut = 160;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

} // namespace UsageTelemetry
} // namespace Slic3r
