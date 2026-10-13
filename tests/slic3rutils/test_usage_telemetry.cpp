#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

std::string slurp(const fs::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every event name in every batch saved for the next launch.
std::vector<std::string> pending_event_names(const fs::path &dir)
{
    std::vector<std::string> names;
    json batches = json::parse(slurp(dir / "usage" / "pending.json"), nullptr, false);
    REQUIRE(batches.is_array());
    for (const auto &body : batches) {
        json batch = json::parse(body.get<std::string>());
        for (const auto &e : batch["events"])
            names.push_back(e["name"].get<std::string>());
    }
    return names;
}

} // namespace

TEST_CASE("Usage diagnostics redact slicing errors", "[UsageTelemetry]")
{
    using UsageTelemetry::redact_message;

    CHECK(redact_message("Nothing to be sliced.", {}) == "Nothing to be sliced.");
    // First line only.
    CHECK(redact_message("Line one\nObject Secret_Bracket has no volume", {}) == "Line one");
    // The model's own names, longest first, wherever they appear.
    CHECK(redact_message("Bracket_v2 and Bracket are too small", {"Bracket", "Bracket_v2"}) == "<name> and <name> are too small");
    // Quoted text and paths.
    CHECK(redact_message("Object \"F-35 mount\" failed", {}) == "Object <...> failed");
    CHECK(redact_message("Cannot write C:\\Users\\someone\\part.gcode now", {}) == "Cannot write <path> now");
    CHECK(redact_message("Cannot write /home/someone/part.gcode now", {}) == "Cannot write <path> now");
    // Numbers and units survive: they are what makes the message useful.
    CHECK(redact_message("Speed 300 mm/s exceeds 250", {}) == "Speed 300 mm/s exceeds 250");
    // Cut to 160 bytes on a character boundary.
    std::string long_utf8;
    for (int i = 0; i < 100; ++i)
        long_utf8 += "\xC3\xA9"; // e acute, 2 bytes
    std::string cut = redact_message(long_utf8, {});
    CHECK(cut.size() == 160);
    CHECK((static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
}

TEST_CASE("Usage diagnostics keep batches and find crashes offline", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    // Nothing listens on the discard port, so every send fails fast and is kept for later.
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    config.set_bool("usage_diagnostics", true);

    UsageTelemetry::start(config, true);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    REQUIRE(UsageTelemetry::enabled());
    const std::string install = config.get("usage_diagnostics_id");
    CHECK(install.size() == 36);

    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::count("tab:preview", 2);
    UsageTelemetry::shutdown();

    CHECK(pending_event_names(dir) == std::vector<std::string>{"slice", "session_end"});
    {
        json batch = json::parse(json::parse(slurp(dir / "usage" / "pending.json"))[0].get<std::string>());
        CHECK(batch["install"] == install);
        CHECK(batch["events"][1]["counts"]["tab:preview"] == 2);
    }
    // A clean exit leaves no crash marker.
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        CHECK(it->path().filename().string().rfind("session-", 0) != 0);

    // A marker from a process that is gone: that session crashed.
    {
        std::ofstream marker((dir / "usage" / "session-dead.json").string());
        marker << json{{"pid", 0x7ffffff0}, {"version", "0.1.1"}, {"started", 1000}, {"alive", 61000}}.dump();
    }
    UsageTelemetry::start(config, false);
    UsageTelemetry::shutdown();

    std::vector<std::string> names = pending_event_names(dir);
    CHECK(std::count(names.begin(), names.end(), "crash") == 1);
    CHECK(!fs::exists(dir / "usage" / "session-dead.json"));
    // The first launch's batch is still waiting too.
    CHECK(std::count(names.begin(), names.end(), "slice") == 1);
    // Same install id across launches.
    CHECK(config.get("usage_diagnostics_id") == install);

    // Switching off forgets everything queued.
    UsageTelemetry::start(config, false);
    UsageTelemetry::set_enabled(config, false);
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}


// UNLAYERED_MOD 2026-10-11 (0.1.4) -- a crash says where it happened. Day one of 0.1.3 reported 17
// "crashes" with nothing to go on: no phase, no exception, no location.
TEST_CASE("Usage diagnostics report a crash's phase, exception and location, and skip OS session ends", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    config.set_bool("usage_diagnostics", true);

    // Two dead sessions: one crashed while slicing, one the operating system ended.
    auto write_marker = [&](const std::string &name, const json &body) {
        std::ofstream out((dir / "usage" / name).string());
        out << body.dump();
    };
    write_marker("session-crashed.json", {{"pid", 0x7ffffff0}, {"version", "0.1.4"}, {"started", 1000}, {"alive", 31000},
                                          {"phase", "slicing"}, {"exception", "0xC0000005"}, {"at", "UnlayeredSlicer.dll+0x1a2b3c"}});
    write_marker("session-logoff.json", {{"pid", 0x7ffffff1}, {"version", "0.1.4"}, {"started", 1000}, {"alive", 31000},
                                         {"phase", "os_session_end"}});

    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }

    // The live session's marker carries the phase the app says it is in.
    UsageTelemetry::set_phase("slicing");
    fs::path live;
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        if (it->path().filename().string().rfind("session-", 0) == 0)
            live = it->path();
    REQUIRE(!live.empty());
    CHECK(json::parse(slurp(live))["phase"] == "slicing");

#ifdef _WIN32
    // A code address is a module file name and an offset, never a path.
    const std::string here = UsageTelemetry::code_location(reinterpret_cast<const void *>(&slurp));
    INFO(here);
    CHECK(here.find("slic3rutils_tests") != std::string::npos);
    CHECK(here.find("+0x") != std::string::npos);
    CHECK(here.find('\\') == std::string::npos);
    CHECK(here.find('/') == std::string::npos);
#endif

    UsageTelemetry::shutdown();

    json crash;
    int  crashes = 0;
    json batches = json::parse(slurp(dir / "usage" / "pending.json"));
    for (const auto &body : batches) {
        // Named, not iterated as a temporary: operator[] on a temporary would dangle.
        const json batch = json::parse(body.get<std::string>());
        for (const auto &e : batch["events"])
            if (e["name"] == "crash") {
                ++crashes;
                crash = e["props"];
            }
    }
    // Only the real crash: the session the operating system ended is not one.
    INFO(slurp(dir / "usage" / "pending.json"));
    CHECK(crashes == 1);
    CHECK(crash["phase"] == "slicing");
    CHECK(crash["exception"] == "0xC0000005");
    CHECK(crash["at"] == "UnlayeredSlicer.dll+0x1a2b3c");
    CHECK(crash["uptime_s"] == 30);
    CHECK(!fs::exists(dir / "usage" / "session-logoff.json"));

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}


// UNLAYERED_MOD 2026-10-11 (0.1.4) -- the update check sends the install id, and only while
// diagnostics are on.
TEST_CASE("Usage diagnostics give the install id out only while on", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    config.set_bool("usage_diagnostics", true);

    CHECK(UsageTelemetry::install_id().empty()); // not started yet
    UsageTelemetry::start(config, true);
    if (UsageTelemetry::disabled_by_environment()) {
        CHECK(UsageTelemetry::install_id().empty());
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    CHECK(UsageTelemetry::install_id() == config.get("usage_diagnostics_id"));
    CHECK(UsageTelemetry::install_id().size() == 36);

    UsageTelemetry::set_enabled(config, false);
    CHECK(UsageTelemetry::install_id().empty());
    UsageTelemetry::shutdown();
    CHECK(UsageTelemetry::install_id().empty());

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Usage diagnostics report Unlayered's own copies as channel dev", "[UsageTelemetry]")
{
    // The site counts only "release" towards customers.
    CHECK(UsageTelemetry::channel_for("0.1.4", false) == "release");
    CHECK(UsageTelemetry::channel_for("0.1.5-beta1", false) == "beta");
    CHECK(UsageTelemetry::channel_for("0.1.5-RC2", false) == "beta");
    CHECK(UsageTelemetry::channel_for("0.2.0-dev", false) == "dev");
    // An internal copy is dev whatever its version says.
    CHECK(UsageTelemetry::channel_for("0.1.4", true) == "dev");
    CHECK(UsageTelemetry::channel_for("0.1.5-beta1", true) == "dev");
    // This test runs from the build tree, which no installer put there.
    CHECK(UsageTelemetry::is_internal_copy());
}

#ifdef _WIN32
// A crash names its callers, not just the faulting address. The child process (hidden test case below)
// starts diagnostics in a scratch data dir and dereferences null three calls deep; the marker it leaves
// behind is what the next launch would report.
namespace {
__declspec(noinline) void crash_level_three(volatile int *target) { *target = 1; }
__declspec(noinline) void crash_level_two(volatile int *target) { crash_level_three(target); *target = 2; }
__declspec(noinline) void crash_level_one(volatile int *target) { crash_level_two(target); *target = 3; }
} // namespace

TEST_CASE("Usage diagnostics crash child", "[.CrashChild]")
{
    char dir[MAX_PATH] = {0};
    REQUIRE(GetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", dir, MAX_PATH) > 0);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS); // no "has stopped working" window
    set_data_dir(dir);
    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    config.set_bool("usage_diagnostics", true);
    UsageTelemetry::start(config, false);
    UsageTelemetry::set_phase("slicing");
    crash_level_one(nullptr);
}

TEST_CASE("Usage diagnostics crash marker names the callers", "[UsageTelemetry]")
{
    if (UsageTelemetry::disabled_by_environment())
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");

    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-crash-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    SetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", dir.string().c_str());

    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" \"[CrashChild]\"";
    STARTUPINFOW        startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    WaitForSingleObject(process.hProcess, 60000);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", nullptr);
    CHECK(exit_code == 0xC0000005);

    json marker;
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        if (it->path().filename().string().rfind("session-", 0) == 0)
            marker = json::parse(slurp(it->path()), nullptr, false);
    REQUIRE(marker.is_object());
    INFO(marker.dump());
    CHECK(marker["exception"] == "0xC0000005");
    CHECK(marker["phase"] == "slicing");
    const std::string at = marker["at"].get<std::string>();
    // The faulting frame, then its callers: all inside this test program, 3+ frames deep.
    size_t frames = 0;
    for (size_t pos = 0; (pos = at.find("slic3rutils_tests.exe+0x", pos)) != std::string::npos; ++pos)
        ++frames;
    CHECK(frames >= 3);
    CHECK(at.find('>') != std::string::npos);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
#endif
