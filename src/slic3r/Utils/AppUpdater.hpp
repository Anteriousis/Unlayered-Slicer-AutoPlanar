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
// the file in place. On success dest_out names the installer, which the caller owns.
bool download_installer(const AppUpdateAsset &asset, boost::filesystem::path &dest_out,
                        AppUpdateProgressFn progress, std::string &error_out);

// Hands the installer to the OS and returns without waiting for it. The caller is expected to shut
// the application down afterwards, because the installer uninstalls the running copy first.
bool launch_installer(const boost::filesystem::path &installer, std::string &error_out);

// Best-effort removal of a directory left behind by download_installer(). Never throws.
void cleanup_download_dir(const boost::filesystem::path &installer);

} // namespace Slic3r

#endif // slic3r_AppUpdater_hpp_
