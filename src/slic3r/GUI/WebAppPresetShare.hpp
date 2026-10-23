#pragma once

// Printer-profile sharing for the Web App Bridge (WEB_APP_BRIDGE.md section 4.4):
// slicer.exportPrinterPreset writes the edited printer preset out as a complete user-preset JSON,
// slicer.installPrinterPreset takes one back in. The bridge methods themselves live in
// WebAppBridge.cpp; everything here is the part worth reviewing on its own -- what a shared
// profile may never carry, the custom-G-code review the user is shown, the confirmation dialog
// and the import.
//
// The rule both directions enforce: a shared profile never carries a print host, its
// credentials, or a path on someone's disk. Export strips them; install strips them AGAIN,
// whatever the file says, so a profile from the web can never point the installer's uploads at
// someone else's machine.

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/string.h>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

namespace GUI {
namespace WebAppPresetShare {

// ---------------------------------------------------------------------------------------------
// Pure policy (no GUI state; unit-tested in tests/slic3rutils/test_webapp_preset_share.cpp)
// ---------------------------------------------------------------------------------------------

// Every key a shared printer profile loses on the way out and again on the way in:
// WebAppSettings::WAB_SECRET_KEYS (print host, credentials, upload switches) plus
// bed_custom_texture / bed_custom_model, which hold absolute paths on the author's disk.
const std::vector<std::string>& stripped_keys();

// Erase stripped_keys() from `config`. Returns how many were present.
size_t sanitise_printer_preset(DynamicPrintConfig& config);

// One custom-G-code line that does something a printer profile has no business doing quietly.
struct FlaggedLine
{
    std::string key;     // the option, e.g. "machine_start_gcode"
    std::string line;    // the line, comment stripped and trimmed
    std::string reasons; // e.g. "saves settings permanently (M500)"; several joined with "; "
};

// Scan one G-code text: per line, strip a ';' comment, then flag every M-code from the risky table
// that appears as a whole token, case-insensitively (M500, m0500 and "{endif}M500" match; M5000
// and M500.1 do not). Appends one entry per flagged line.
void scan_gcode_text(const std::string& key, const std::string& text, std::vector<FlaggedLine>& out);
// Every string option of `config` whose key ends in "_gcode", in key order.
std::vector<FlaggedLine> scan_custom_gcode(const ConfigBase& config);

// The macro files the G-code runs with M98 P"<file>" (or an unquoted P<file>), sorted, unique.
void                     collect_called_macros(const std::string& text, std::vector<std::string>& out);
std::vector<std::string> called_macros(const ConfigBase& config);

// "<key>: <line> — <reasons>" per entry, at most `cap` of them, then "and N more".
std::vector<std::string> format_flagged(const std::vector<FlaggedLine>& flagged, size_t cap = 20);

// Usable as a preset name: non-empty, no path separators, reserved or control characters, and
// not "." or "..". The importer refuses a name escaping the preset folder silently; this lets the
// install say so.
bool preset_name_usable(const std::string& name);

// ---------------------------------------------------------------------------------------------
// GUI thread
// ---------------------------------------------------------------------------------------------

// slicer.exportPrinterPreset. The EDITED printer preset (what the sidebar shows, unsaved edits
// included), flattened and sanitised, written as a user-preset JSON to `out_path`.
struct ExportResult
{
    int         code{0};   // WAB_OK, or the bridge error code to answer with
    std::string message;   // reply text when code != WAB_OK
    std::string name;      // the preset's name, as written into the file
    std::string inherits;  // its parent, kept as is ("" for a custom root)
    bool        dirty{false};
};
// The name the file will carry, for the caller to build `out_path` from. Empty when no printer
// preset is selected.
std::string   edited_printer_preset_name();
ExportResult  export_edited_printer_preset(const boost::filesystem::path& out_path);

// slicer.installPrinterPreset, after the download. Parses `file`, sanitises it, resolves its
// parent, shows the confirmation dialog (titled after `tab_title`), imports, refreshes and selects
// the result. `work_dir` receives (and loses again) the temporary sanitised JSON.
struct InstallResult
{
    bool        ok{false};
    bool        cancelled{false};
    int         code{0};    // bridge error code for a failure; 0 for success or a user cancel
    std::string name;       // the installed preset's name
    std::string selected;   // the printer preset in force afterwards
    std::string error;
};
InstallResult install_printer_preset(const boost::filesystem::path& file, const wxString& tab_title,
                                     const boost::filesystem::path& work_dir);

} // namespace WebAppPresetShare
} // namespace GUI
} // namespace Slic3r
