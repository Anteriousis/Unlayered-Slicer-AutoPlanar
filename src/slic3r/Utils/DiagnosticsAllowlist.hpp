#ifndef slic3r_Utils_DiagnosticsAllowlist_hpp_
#define slic3r_Utils_DiagnosticsAllowlist_hpp_

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {

// What usage diagnostics may carry, enforced at the one place every event passes through
// (UsageTelemetry::record and the crash / session_end events it makes itself). 0.1.6.
//
// An allowlist, not a blocklist: an event name that is not listed is dropped whole, a property that
// is not listed for its event is dropped, and a tally (counts) must start with a listed prefix.
// Every string that survives (alone or in a list) must also look like a value, not like the
// person's work: no paths, no file names of models, projects or G-code, no e-mail addresses, no
// line breaks, nothing longer than the event allows. A value that fails is removed, not
// rewritten, so a leak can never be "almost" redacted.
//
// Adding a diagnostic means adding it here, in USAGE_DIAGNOSTICS.md and in the privacy policy.
namespace DiagnosticsAllowlist {

// Is `name` a known event?
bool known_event(const std::string &name);

// Does this string look safe to send? (No path, model/project/G-code file name, e-mail, line break;
// at most `max_len` bytes.)
bool safe_string(const std::string &value, size_t max_len = 190);

// The event's properties and tallies with everything not allowed removed. Returns false (and
// leaves the outputs empty) when the event itself is not allowed.
bool filter(const std::string &name, const nlohmann::json &props, const nlohmann::json &counts, nlohmann::json &props_out,
            nlohmann::json &counts_out);

// A tally name, as count() and the "counts" of an event use them: "tab:preview", "parts:ironing".
bool allowed_count_key(const std::string &key);

} // namespace DiagnosticsAllowlist
} // namespace Slic3r

#endif // slic3r_Utils_DiagnosticsAllowlist_hpp_
