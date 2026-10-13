#ifndef slic3r_AppUpdater_hpp_
#define slic3r_AppUpdater_hpp_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Where users are sent when the update manifest does not name a download page of its own, so the
// dialog's confirm button is never a no-op.
constexpr const char *UNLAYERED_DOWNLOAD_PAGE_URL = "https://unlayered3d.com/slicer/";

// One downloadable file advertised by the update manifest. The field names mirror the GitHub
// releases API because the manifest is deliberately shaped like one - see AppConfig's
// VERSION_CHECK_URL for the contract.
struct AppUpdateAsset
{
    std::string name;
    std::string url;
    std::string sha256;   // lowercase hex; empty means "not declared", verification is skipped
    uint64_t    size{0};  // bytes; 0 means "not declared"
};

// One entry of the manifest's `unlayered_downloads`: an installer behind the Unlayered account
// (see UnlayeredAccount). `url` is NOT the file - it answers, given the account's device token,
// with a short-lived signed link to it.
struct AccountDownload
{
    std::string platform; // "windows" | "mac" | "linux"
    std::string arch;     // "x64" | "arm64"
    std::string name;
    std::string url;
    std::string sha256;
    uint64_t    size{0};
};

// The platform name the update check sends as ?platform= and the manifest uses.
const char *updater_platform();
// "x64" or "arm64", as the manifest spells it.
const char *updater_arch();

// Picks the account download for this platform and architecture. Windows only, like
// select_platform_asset(), and for the same reason. Refuses an entry without a sha256: an
// installer that cannot be verified is never run unattended.
std::optional<AccountDownload> select_account_download(const std::vector<AccountDownload> &downloads);

// Platform-specific rollouts: whether a manifest release was published for this platform, so that
// publishing a Mac-only 0.1.5 does not announce 0.1.5 to Windows or Linux. True when the release
// lists an installer for this platform: an unlayered_downloads entry naming it, or an asset of its
// file type (.exe/.msi on Windows, .dmg on macOS, .AppImage on Linux). A release that lists no
// installers at all (a page-only manifest) is still true, as before, so an old manifest cannot
// silently stop updates. Unlike the select_* functions this does not ask whether the update can be
// installed in place, only whether it exists for this platform.
bool release_targets_platform(const std::vector<AppUpdateAsset> &assets, const std::vector<AccountDownload> &downloads);

// Where this copy is installed, if - and only if - it can update itself without asking: a per-user
// installation made by our NSIS installer (its uninstaller sits next to the executable and the
// current user's registry hive names this folder). A Program Files install, a portable copy, a
// development build or another platform return nothing, and get the interactive path instead.
std::optional<boost::filesystem::path> self_updatable_install_dir();

// True when another process is running this same executable. A silent install would fail on its
// locked files, so the update waits until it is the last window.
bool other_instances_running();

// Picks the asset that can be installed on the platform this binary was built for, or nothing.
//
// Returning nothing is a normal outcome, not an error: it happens whenever the manifest carries no
// assets (the state while downloads sit behind a sign-in), whenever no asset matches this
// platform, and on every platform where in-app installation is not implemented. Callers fall back
// to opening the download page in a browser.
std::optional<AppUpdateAsset> select_platform_asset(const std::vector<AppUpdateAsset> &assets);

// Reports download progress. Returning false cancels. Mirrors FirmwareProgressFn.
using AppUpdateProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

// Downloads the asset, verifies the size and checksum the manifest declared, and only then puts
// the file in place. On success dest_out names the installer, which the caller owns. Safe to call
// from a worker thread when `progress` is (or is empty).
bool download_installer(const AppUpdateAsset &asset, boost::filesystem::path &dest_out,
                        AppUpdateProgressFn progress, std::string &error_out);

// Hands the installer to the OS and returns without waiting for it. The caller is expected to shut
// the application down afterwards, because the installer uninstalls the running copy first.
bool launch_installer(const boost::filesystem::path &installer, std::string &error_out);

// Starts the installer with no UI, to replace the copy in `install_dir` once this process has
// exited: it waits on this process's id first, so call it as the very last thing before exiting.
// With `relaunch` the installer starts the application again when it is done.
bool launch_silent_update(const boost::filesystem::path &installer, const boost::filesystem::path &install_dir,
                          bool relaunch, std::string &error_out);

// Best-effort removal of a directory left behind by download_installer(). Never throws.
void cleanup_download_dir(const boost::filesystem::path &installer);

// Best-effort removal of download directories left by earlier runs (a crashed session, or an
// installer that has since finished). Never touches this process's own directory. Never throws.
void cleanup_stale_download_dirs();

} // namespace Slic3r

#endif // slic3r_AppUpdater_hpp_
