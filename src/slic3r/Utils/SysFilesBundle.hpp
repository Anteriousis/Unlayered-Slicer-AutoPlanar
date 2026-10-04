#ifndef slic3r_SysFilesBundle_hpp_
#define slic3r_SysFilesBundle_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// The printer's configuration - config.g, the homing and tool-change macros, the macros the user
// presses in DWC, filament scripts - lives as ordinary files on the board's SD card. This is the
// firmware updater's counterpart for those: it takes a set of files from a release or from a
// folder on disk and writes them to the card over the same rr_* HTTP API.
//
// Deliberately not part of FirmwareBundle: an image is flashed by the board and identified by the
// object model, whereas these files are just copied, and nothing on the board describes what
// version of them is installed. That last gap is closed by a marker file this code writes.

// Card-relative path of the marker recording what was last installed. It is written after a
// successful install and read back to answer "what is on this printer?"; nothing depends on it
// being present, so a hand-loaded card simply reports an unknown version.
extern const char *SYS_MARKER_PATH; // "sys/unlayered-sys.json"

// One file destined for the SD card.
struct SysFile
{
    // Card-relative, always forward slashes, always under one of the allowed roots, e.g.
    // "sys/config.g" or "macros/Load Filament".
    std::string          path;
    std::string          sha256;  // lowercase hex; empty means "not declared"
    std::vector<uint8_t> data;
    // Where this came from, for the UI and the log: a zip entry name or a path on disk.
    std::string          origin;

    std::string remote_path() const { return "0:/" + path; }
};

struct SysFilePackage
{
    // Free-form, whatever the package calls itself. Empty means "unknown", which is never treated
    // as up to date - the same rule the firmware updater applies to a hand-picked file.
    std::string version;
    std::string date;
    // Which machine the package was written for. Shown to the user and recorded in the marker,
    // never enforced: one Duet board model covers every Unlayered machine, so there is nothing on
    // the printer that could be checked against it without lying about the strength of the check.
    std::string machine;
    std::string notes;
    std::string origin;   // human readable source description

    std::vector<SysFile> files;

    // Files that were found but left out, with the reason. Surfaced rather than dropped quietly:
    // "it did not upload my file" is otherwise impossible to diagnose.
    std::vector<std::pair<std::string, std::string>> skipped;

    uint64_t       total_bytes() const;
    const SysFile *find(const std::string &path) const;
};

// What the marker file on the card says. All fields are empty/zero when there is no marker.
struct SysFilesInstalled
{
    bool        known = false;
    std::string version;
    std::string date;         // the package's own date
    std::string machine;
    std::string installed_at; // when this slicer wrote the files
    std::string origin;
    int         file_count = 0;
};

// The directories a package is allowed to write. 0:/firmware is deliberately absent: that is the
// firmware updater's business, and 0:/gcodes is not configuration.
const std::vector<std::string> &sys_allowed_roots(); // {"sys", "macros", "filaments"}

// Normalises a path taken from an archive or from disk (separators, "./", duplicate slashes) and
// maps it onto a card-relative path: a path already rooted at one of the allowed directories is
// kept, anything else is placed in sys/. Returns false, with a reason, for a path that escapes the
// card (".." components, absolute paths, drive letters).
bool sys_map_path(const std::string &relative, std::string &card_path_out, std::string &reason_out);

// True when a path is a file the packaging should ignore rather than upload: editor and backup
// droppings, VCS metadata, and the marker file, which this code owns.
bool sys_is_ignored_file(const std::string &relative_path);

// Parses the manifest.json carried by a package:
//   { "version": "...", "date": "...", "machine": "...", "notes": "...",
//     "files": [ { "path": "sys/config.g", "sha256": "..." }, ... ] }
// Everything except "version" is optional. "files" is optional too; when it is present it is
// authoritative and anything else in the package is left alone.
bool parse_sys_manifest(const std::string &json, SysFilePackage &pkg_out, std::string &error_out);

// Verifies declared checksums and that every target path is inside an allowed root.
bool validate_sys_package(const SysFilePackage &pkg, std::string &error_out);

// The JSON written to SYS_MARKER_PATH after a successful install.
std::string make_sys_marker_json(const SysFilePackage &pkg, const std::vector<std::string> &installed_paths);
bool        parse_sys_marker(const std::string &json, SysFilesInstalled &out);

// Reports progress. Returning false cancels the fetch.
using SysProgressFn = std::function<bool(const std::string &stage, uint64_t done, uint64_t total)>;

class SysFileSource
{
public:
    virtual ~SysFileSource() = default;
    // Retrieves the metadata and every file payload. Payloads here are a few kilobytes of G-code,
    // so unlike firmware there is no reason to separate a metadata-only pass.
    virtual bool fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) = 0;
    virtual std::string description() const = 0;
};

// A folder or a .zip on disk. This is the bench workflow: point it at a machine's sys directory,
// or at a zip built by tools/make_sys_bundle.py, and push.
class LocalSysFileSource : public SysFileSource
{
public:
    explicit LocalSysFileSource(std::string path);
    bool        fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) override;
    std::string description() const override;

private:
    std::string m_path;

    bool fetch_from_zip(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out);
    bool fetch_from_directory(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out);
};

// A GitHub release carrying a zip of the configuration. Same reasoning as the firmware source: it
// decouples a config fix from a slicer release.
//
// The release is expected to have one zip asset (named sys.zip when there are several). A
// manifest.json inside the zip supplies the version, notes and checksums; without one the release
// tag stands in for the version and every file in the zip is installed.
class GitHubSysFileSource : public SysFileSource
{
public:
    struct Options
    {
        // Base of the releases API. Overridable so the updater can be pointed at a local server
        // for testing, or at our own host if these artifacts stop being public. Anything serving
        // the GitHub releases JSON shape works; no trailing slash.
        std::string api_base = "https://api.github.com";
        std::string owner = "Unlayered3D";
        std::string repo  = "printer-config";
        // Empty means "the newest published release".
        std::string tag;
        bool        allow_prerelease = true;
        // Optional token for a private repository. Prefer a public repository over shipping one.
        std::string token;
        // Preferred asset name; any .zip asset is accepted when this one is not present.
        std::string asset_name = "sys.zip";
    };

    explicit GitHubSysFileSource(Options opts);
    bool        fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) override;
    std::string description() const override;

private:
    Options m_opts;
};

} // namespace Slic3r

#endif // slic3r_SysFilesBundle_hpp_
