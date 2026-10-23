#ifndef slic3r_FirmwareBundle_hpp_
#define slic3r_FirmwareBundle_hpp_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "PrinterUpdateCheck.hpp"

namespace Slic3r {

class DuetClient;

// Which RRF update module an image belongs to. The numeric values are the S parameter of M997
// as defined by RepRapFirmware's FirmwareUpdater.h, except for Iap which is never flashed
// directly - it is the programmer that performs the mainboard flash.
//
// The two display roles are alternatives, never both on one machine: PanelDue is module 4
// (SAM-BA, erase-before-receive) and PanelOta is module 5, the Unlayered ESP32 panel, which
// writes the inactive OTA slot and rolls back on its own. A release carries both images and
// the job flashes whichever matches the display actually attached - see display_role_for().
//
// Www is not firmware and not an M997 module at all: it is the web interface (Unlayered Web
// Control, our DuetWebControl build) as an SD-card zip, unpacked into 0:/www/. It travels with the
// firmware because DWC compares its own version with the board's and warns on any difference, and
// stock DWC reads "+unlayered.N" as a mismatch - so each release carries a UWC stamped with exactly
// its firmware version, installed after the mainboard flash. See FirmwareUpdateJob.
enum class FirmwareRole { Mainboard, Iap, Wifi, PanelDue, PanelOta, Www, Unknown };

int         firmware_role_to_module(FirmwareRole role); // -1 for Iap/Unknown
FirmwareRole firmware_role_from_string(const std::string &s);
const char  *firmware_role_to_string(FirmwareRole role);

// Which display is attached to aux0, and how confidently we know.
//
// RRF has no object-model key for the device on an aux port, so the machine publishes its own
// identity instead. There are two sources, same encoding (1 = PanelDue, 2 = ESP32 panel):
//
//   global.displayId    0:/sys/displayid.g, called by the display itself on connect. What the
//                       HARDWARE reports, so the higher authority - but deliberately NOT
//                       persisted, so it is absent until a display announces itself in the
//                       current boot. A working, fully commissioned machine can lack it: .90 has
//                       an active panel on aux0 and no displayId at all.
//   global.displayType  0:/sys/display.g, written by the setup wizard and run by config.g at
//                       boot. The OPERATOR'S confirmed answer, and persisted, so it is there on
//                       every commissioned machine. Also the only one that can say 0 = none.
//
// displayId wins when present because it is evidence from the device; displayType is what makes
// the answer available at all on most machines.
//
// The old tell - inputs[4].lineNumber climbing, because a PanelDue sends line-numbered commands
// and the panel did not - is deliberately NOT used. The 4.3in panel sets checksums on both wiring
// branches as of 2026-09-09, so it numbers its lines exactly like a PanelDue and the heuristic now
// returns PanelDue for either device. A wrong answer here costs a timeout rather than a brick
// (a PanelDue ignores the panel's fwPush announce and the panel ignores eraseAndReset), but
// guessing silently would still flash the wrong image and report success.
struct DisplayInfo
{
    // PanelDue, PanelOta, or Unknown when the machine did not say.
    FirmwareRole role    = FirmwareRole::Unknown;
    bool         certain = false;
    // The printer positively stated it has NO display (global.displayType = 0). Distinct from
    // Unknown, which only means nothing could be established.
    bool         none    = false;
    std::string  evidence;   // shown in the dialog, so a user can sanity-check what we concluded
};

// Reads global.displayId, falling back to global.displayType. Free of side effects on purpose -
// this runs every time the dialog opens, so it must never probe with M999, which resets the panel.
DisplayInfo detect_display(DuetClient &client);

struct FirmwareImage
{
    FirmwareRole         role = FirmwareRole::Unknown;
    std::string          filename;   // target name inside 0:/firmware/
    std::string          sha256;     // lowercase hex; empty means "not declared"
    std::vector<uint8_t> data;       // populated once the image has been fetched

    bool loaded() const { return !data.empty(); }
};

struct FirmwarePackage
{
    // Exact firmware version string as reported by the board, e.g. "3.7.0-beta.1+unlayered.1".
    // Compared for exact equality - never with Semver, whose comparison ignores build metadata
    // and would treat +unlayered.1 and +unlayered.2 as the same version.
    std::string version;
    std::string build_date;   // optional; disambiguates rebuilds that share a version string
    std::string board_short;  // e.g. "Mini5plus"; matched against boards[0].shortName
    std::string notes;        // release notes shown in the confirmation dialog
    std::string origin;       // human readable source description, for logs and the UI

    std::vector<FirmwareImage> images;

    const FirmwareImage *find(FirmwareRole role) const;
    FirmwareImage       *find(FirmwareRole role);
    uint64_t             total_bytes() const;
};

// True when the payload starts with the UF2 magic numbers. Catches "right slicer, wrong file"
// before anything is written to the board; RRF performs its own check, but only after the file
// has already been uploaded to the SD card.
bool is_uf2_image(const std::vector<uint8_t> &data);

// True when the payload starts with a zip local-file header, PK 03 04. The web interface check.
bool is_zip_archive(const std::vector<uint8_t> &data);

// Lowercase hex SHA-256 of a buffer.
std::string sha256_hex(const std::vector<uint8_t> &data);

// Verifies declared checksums, UF2 magic and board match. Returns false and fills error_out on
// the first problem found.
bool validate_package(const FirmwarePackage &pkg, const std::string &board_short_name, std::string &error_out);

// Parses the manifest.json format shared by every source:
//   { "version": "...", "buildDate": "...", "board": "Mini5plus", "notes": "...",
//     "images": [ { "role": "iap", "file": "...", "sha256": "..." }, ... ] }
// Image payloads are not populated here - only the metadata.
bool parse_manifest(const std::string &json, FirmwarePackage &pkg_out, std::string &error_out);

// -----------------------------------------------------------------------------------------------
// Web interface (role Www)
// -----------------------------------------------------------------------------------------------

// Where the web interface lives on the card. RRF serves 0:/www/<path>, preferring <path>.gz.
extern const char *const WEB_INTERFACE_ROOT; // "0:/www"

struct WebInterfaceFile
{
    std::string          path;   // relative to 0:/www, forward slashes, e.g. "js/app-D_vsAdMJ.js.gz"
    std::vector<uint8_t> data;
};

// Unpacks a web interface SD zip (DuetWebControl-SD.zip layout: index.html.gz, css/, js/, fonts/
// at the root) and checks it is one before anything touches the printer:
//  - it must contain index.html.gz or index.html at its root;
//  - every path must be relative, without "..", and limited to letters, digits and "._-/" - the
//    names go into rr_* URLs and, over USB, into G-code lines (M30 takes an unquoted path);
//  - sizes are bounded, so a wrong zip fails here rather than streaming for minutes.
// Zero-byte files are skipped (rr_upload cannot create one). The result is in upload order - see
// web_interface_upload_order().
bool extract_web_interface(const std::vector<uint8_t> &zip_bytes, std::vector<WebInterfaceFile> &files_out,
                           std::string &error_out);

// The order the files go to the card: everything else sorted by path, then index.html, then
// index.html.gz LAST. The index is the switch-over moment - until it lands, a browser still loads
// the old index, whose hashed asset names are still on the card - so an install that stops part
// way never serves a new index that references files which never arrived.
void web_interface_upload_order(std::vector<WebInterfaceFile> &files);

// Paths (relative to 0:/www) that were on the card before the install and are not part of the new
// bundle, i.e. what to delete afterwards. FAT is case-insensitive, so the comparison is too:
// "Index.html.gz" on the card IS the "index.html.gz" just uploaded and must never be deleted.
std::vector<std::string> stale_web_interface_files(const std::vector<std::string> &old_files,
                                                   const std::vector<WebInterfaceFile> &new_files);

// Directories (relative to 0:/www) that held old files and hold none of the new ones, deepest
// first, so deleting them in order removes nested empty folders before their parents.
std::vector<std::string> stale_web_interface_dirs(const std::vector<std::string> &old_dirs,
                                                  const std::vector<WebInterfaceFile> &new_files);

// Every directory the bundle needs (relative to 0:/www), parents before children.
std::vector<std::string> web_interface_dirs(const std::vector<WebInterfaceFile> &files);

// Reports download/extract progress. Returning false cancels the fetch.
using FirmwareProgressFn = std::function<bool(const std::string &stage, uint64_t done, uint64_t total)>;

class FirmwareSource
{
public:
    virtual ~FirmwareSource() = default;
    // Retrieves metadata and all image payloads.
    virtual bool fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out) = 0;
    // Retrieves just enough to answer "what version is available, and what changed?" without
    // transferring any image payloads. Sources for which that distinction is meaningless simply
    // do the full fetch.
    virtual bool fetch_metadata(FirmwarePackage &pkg_out, std::string &error_out)
    {
        return fetch(pkg_out, nullptr, error_out);
    }
    virtual std::string description() const = 0;
};

// Reads resources/firmware/manifest.json shipped alongside the application. Offline fallback and
// a guaranteed-known-good image, at the cost of only changing when the slicer is reinstalled.
class BundledFirmwareSource : public FirmwareSource
{
public:
    BundledFirmwareSource();
    explicit BundledFirmwareSource(std::string directory);
    bool fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out) override;
    std::string description() const override;
    // True when a manifest is actually present in the installation.
    bool available() const;

private:
    std::string m_directory;
};

// A single user-picked .uf2/.bin, or a web interface zip (UnlayeredWebControl*.zip or
// DuetWebControl*.zip). The role is inferred from the file name against what the board reported,
// so a locally built image drops straight in. This is the bench workflow.
class LocalFileFirmwareSource : public FirmwareSource
{
public:
    // board_firmware_filename / board_iap_filename come from the object model and are used both
    // to infer the role and to name the file on the SD card.
    LocalFileFirmwareSource(std::string path,
                            std::string board_firmware_filename,
                            std::string board_iap_filename,
                            std::string board_wifi_filename);
    bool fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out) override;
    std::string description() const override;

private:
    std::string m_path;
    std::string m_board_firmware_filename;
    std::string m_board_iap_filename;
    std::string m_board_wifi_filename;
};

// Fetches a GitHub release and its assets. This is the primary source: it decouples firmware
// releases from slicer releases, so a firmware fix does not require shipping a new installer.
//
// Two layouts are supported, in order:
//   1. The release carries a "manifest.json" asset - roles, checksums and board are read from it.
//   2. No manifest - assets are matched by name against the filenames the board reported, the
//      same trick DuetWebControl uses. This makes "just attach the .uf2 to a release" work.
class GitHubReleaseFirmwareSource : public FirmwareSource
{
public:
    struct Options
    {
        // Base of the releases API. Overridable so the updater can be pointed at a local server
        // for testing, or at our own host if these artifacts stop being public. Anything serving
        // the GitHub releases JSON shape works; no trailing slash.
        std::string api_base = "https://api.github.com";
        std::string owner = "Unlayered3D";
        std::string repo  = "RepRapFirmware";
        // Empty means "latest release". Otherwise an exact tag.
        std::string tag;
        // Include releases marked as pre-release when resolving "latest".
        bool allow_prerelease = true;
        // Optional credential. The firmware feed is public and the application leaves this empty;
        // when set, it is attached only to requests on its own site (FeedCredential::token_for).
        FeedCredential credential;
        // Filenames the board reported, used for the no-manifest fallback and to name uploads.
        std::string board_firmware_filename;
        std::string board_iap_filename;
        std::string board_wifi_filename;
        std::string board_short_name;
    };

    explicit GitHubReleaseFirmwareSource(Options opts);
    bool fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out) override;
    std::string description() const override;

    // Resolves the release and its metadata without downloading any image payloads. Used for a
    // cheap "is there anything newer?" check.
    bool fetch_metadata(FirmwarePackage &pkg_out, std::string &error_out) override;

private:
    Options m_opts;
    // asset name -> download URL, populated by fetch_metadata().
    std::vector<std::pair<std::string, std::string>> m_assets;

    bool download_asset(const std::string &url, const std::string &name,
                        std::vector<uint8_t> &out, FirmwareProgressFn progress, std::string &error_out);
};

// Tries each source in turn and uses the first that works, so a printer can still be updated
// from what shipped with the installer when GitHub is unreachable, rate limited, or simply has
// no release published yet.
class ChainedFirmwareSource : public FirmwareSource
{
public:
    void add(std::unique_ptr<FirmwareSource> source);
    bool fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out) override;
    bool fetch_metadata(FirmwarePackage &pkg_out, std::string &error_out) override;
    std::string description() const override;
    bool empty() const { return m_sources.empty(); }

private:
    std::vector<std::unique_ptr<FirmwareSource>> m_sources;
    // Description of the source that last satisfied a request, for the UI and logs.
    mutable std::string m_used;
};

} // namespace Slic3r

#endif // slic3r_FirmwareBundle_hpp_
