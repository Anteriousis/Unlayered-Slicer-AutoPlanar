#ifndef slic3r_Utils_UsageTelemetry_hpp_
#define slic3r_Utils_UsageTelemetry_hpp_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class AppConfig;
class Preset;
class PresetCollection;

// Anonymous usage diagnostics: what people use, which machines they run on, what fails.
//
// Events are batched in memory and POSTed as JSON to https://unlayered3d.com/api/slicer/telemetry
// (the site's app/lib/slicerTelemetry.ts validates and aggregates them; /admin/telemetry shows
// them). On by default, switched off in Preferences > Online > "Send anonymous usage diagnostics",
// and forced off on a machine by setting the environment variable UNLAYERED_NO_TELEMETRY to
// anything non-empty, for sites that deploy the slicer centrally.
//
// The one rule every caller keeps: nothing that comes from the person's work leaves the machine.
// No model geometry or G-code, no file, object, project or user-preset names, no paths, no setting
// VALUES typed in by hand (setting NAMES are fine), no account. A batch carries a random
// per-install id, a random per-session id, the app version and platform, and named events whose
// properties are short strings, numbers, booleans and lists of short strings.
//
// Crashes are detected on the next launch: each running session keeps a small marker file with
// its process id, deleted on a clean exit; a marker whose process is gone means that session died.
//
// Thread safety: record()/count() may be called from any thread. start(), set_enabled() and
// shutdown() run on the main thread (they touch AppConfig).
namespace UsageTelemetry {

// Once, on the main thread, after AppConfig is loaded. `first_run`: the app config did not exist
// before this launch (a new install). Detects a previous crash and loads unsent batches.
void start(AppConfig &config, bool first_run);

bool enabled();
// From Preferences. Switching off drops everything queued and on disk.
void set_enabled(AppConfig &config, bool on);
// True when UNLAYERED_NO_TELEMETRY switched it off, so Preferences can say why the box is locked.
bool disabled_by_environment();

bool is_first_run();

// Queues one event. `props` must be a JSON object of short strings, numbers, booleans and lists.
// `counts` (optional) is an object of non-negative integer tallies the site adds into its feature
// totals, e.g. {"parts:zaa_enabled": 4}.
void record(const std::string &name, nlohmann::json props = nlohmann::json::object(), nlohmann::json counts = nlohmann::json());

// Bumps a per-session tally (sent once, with session_end): "tab:preview", "gizmo:cut", ...
void count(const std::string &feature, int by = 1);

// Sends what is queued, in the background. Called every few minutes by the app.
void flush();

// At exit: records session_end, sends it with a short timeout (or saves it for the next launch),
// and removes this session's crash marker.
void shutdown();

// What a preset may be called in a report: its own name if it is a system preset, else the system
// preset it inherits from, else "custom". The names people give their own presets are never sent.
std::string preset_base_name(const PresetCollection &collection, const Preset &preset);

// A slicing error message made safe to send: its first line, without quoted text, paths, or any
// of `names` (the model's object and volume names), cut to 160 characters.
std::string redact_message(const std::string &message, const std::vector<std::string> &names);

} // namespace UsageTelemetry
} // namespace Slic3r

#endif // slic3r_Utils_UsageTelemetry_hpp_
