#include "AppUpdater.hpp"

#include <algorithm>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/platinfo.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/FirmwareBundle.hpp" // sha256_hex
#include "slic3r/Utils/Http.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {

namespace {

// Identifies this build to the update host. Kept identical to the token FirmwareBundle uses so
// both outbound Unlayered requests can be filtered out of an access log the same way.
std::string updater_user_agent() { return std::string("UnlayeredSlicer/") + UNLAYERED_VERSION; }

std::string detect_updater_arch()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#else
    std::string arch = wxPlatformInfo::Get().GetArchName().ToStdString();
    boost::algorithm::to_lower(arch);
    if (arch.find("aarch64") != std::string::npos || arch.find("arm64") != std::string::npos)
        return "arm64";
    if (arch.find("x86_64") != std::string::npos || arch.find("amd64") != std::string::npos)
        return "x86_64";
    if (arch.find("i686") != std::string::npos || arch.find("i386") != std::string::npos ||
        arch.find("x86") != std::string::npos)
        return "i386";
    return "unknown";
#endif
}

} // namespace

std::optional<AppUpdateAsset> select_platform_asset(const std::vector<AppUpdateAsset> &assets)
{
#if defined(_WIN32)
    const bool want_arm = (detect_updater_arch() == "arm64");

    // Two passes: an exact architecture match first, then the x64 default. Never cross-install -
    // an arm64 machine must not silently receive an x64 installer, nor the reverse.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto &a : assets) {
            const std::string n = boost::algorithm::to_lower_copy(a.name);

            // Only an installer can replace an installed copy of itself. The portable zip, the
            // debug symbol archive and the profile validator are all published alongside it.
            if (!boost::algorithm::iends_with(n, ".exe") && !boost::algorithm::iends_with(n, ".msi"))
                continue;
            if (n.find("portable") != std::string::npos || n.find("pdb") != std::string::npos ||
                n.find("validator") != std::string::npos)
                continue;

            const bool is_arm = n.find("arm64") != std::string::npos || n.find("_arm") != std::string::npos;
            if (pass == 0 ? (is_arm != want_arm) : (is_arm || want_arm))
                continue;

            return a;
        }
    }
    return std::nullopt;
#else
    // macOS and Linux fall back to the download page. A .dmg has to be mounted and dragged to
    // /Applications, and an AppImage has to be made executable and swapped in place - possibly on
    // a read-only mount. Neither is a two-line launch, and neither is the primary platform.
    (void) assets;
    return std::nullopt;
#endif
}

bool download_installer(const AppUpdateAsset &asset, fs::path &dest_out, AppUpdateProgressFn progress,
                        std::string &error_out)
{
    // A private subdirectory rather than the temp root: the file name comes from the manifest, and
    // a predictable path in a world-writable directory is a swap target between verify and launch.
    const fs::path dir = fs::temp_directory_path() /
                         (std::string("unlayered-update-") + std::to_string(get_current_pid()));

    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error_out = (boost::format("Could not create a download directory: %1%") % ec.message()).str();
        return false;
    }

    // Never trust the manifest for a path component.
    std::string leaf = fs::path(asset.name).filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..")
        leaf = "UnlayeredSlicerSetup.exe";
    const fs::path target = dir / leaf;
    const fs::path tmp    = fs::path(target.string() + ".part");

    BOOST_LOG_TRIVIAL(info) << "updater: downloading " << asset.name << " (" << asset.size
                            << " bytes declared) from " << asset.url;

    bool        ok        = false;
    bool        cancelled = false;
    std::string payload;

    auto http = Http::get(asset.url);
    http.timeout_connect(15)
        // 0 means "no cap". A large download on a slow link is not an error, and cancellation is
        // driven by on_progress rather than a wall clock.
        .timeout_max(0)
        .size_limit(1024ull * 1024 * 1024)
        .header("User-Agent", updater_user_agent())
        .header("Accept", "application/octet-stream")
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && !progress(p.dlnow, p.dltotal ? p.dltotal : asset.size)) {
                cancel    = true;
                cancelled = true;
                error_out = "Cancelled";
            }
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            if (!cancelled)
                error_out = err.empty() ? (boost::format("HTTP %1%") % status).str() : err;
        })
        .on_complete([&](std::string body, unsigned) {
            payload = std::move(body);
            ok      = true;
        })
        .perform_sync();

    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }

    // Verify before anything is written, not after. The bytes are already in memory, so checking
    // them here means a corrupt download never becomes an executable file on disk, not even
    // briefly.
    if (asset.size != 0 && payload.size() != asset.size) {
        error_out = (boost::format("The download is %1% bytes but the update manifest declared %2%. "
                                   "Aborting rather than running a file that is not what was published.") %
                     payload.size() % asset.size)
                        .str();
        fs::remove_all(dir, ec);
        return false;
    }
    if (!asset.sha256.empty()) {
        const std::vector<uint8_t> bytes(payload.begin(), payload.end());
        const std::string          actual = sha256_hex(bytes);
        if (!boost::algorithm::iequals(actual, asset.sha256)) {
            error_out = (boost::format("The download does not match the checksum in the update "
                                       "manifest (expected %1%, got %2%).") %
                         asset.sha256 % actual)
                            .str();
            fs::remove_all(dir, ec);
            return false;
        }
    } else {
        BOOST_LOG_TRIVIAL(warning) << "updater: the manifest declared no sha256 for " << asset.name
                                   << "; installing without checksum verification";
    }

    // Write to .part and rename, so a partially written file is never launchable. The stream state
    // is checked after flushing: without that a full disk yields a silently truncated installer.
    {
        fs::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.good()) {
            error_out = (boost::format("Could not open %1% for writing") % tmp.string()).str();
            fs::remove_all(dir, ec);
            return false;
        }
        file.write(payload.data(), payload.size());
        file.flush();
        if (!file.good()) {
            error_out = "Writing the downloaded installer failed - the disk may be full.";
            file.close();
            fs::remove_all(dir, ec);
            return false;
        }
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        error_out = (boost::format("Could not move the installer into place: %1%") % ec.message()).str();
        fs::remove_all(dir, ec);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "updater: installer ready at " << target.string();
    dest_out = target;
    return true;
}

bool launch_installer(const fs::path &installer, std::string &error_out)
{
#if defined(_WIN32)
    // The wstring must outlive the ShellExecuteExW call, so it is held in a named local. Assigning
    // lpFile straight from a path conversion leaves a dangling pointer.
    const std::wstring file = installer.wstring();

    SHELLEXECUTEINFOW info = {};
    info.cbSize            = sizeof(info);
    // NOASYNC because this process exits immediately afterwards: without it the shell may not have
    // finished handing the request off.
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    // The default verb, not "runas": the NSIS installer's own manifest requests administrator
    // rights, so the shell raises the consent dialog by itself.
    info.lpVerb = nullptr;
    info.lpFile = file.c_str();
    // Interactive rather than /S. The installer removes the previous version before installing the
    // new one, and NSIS silent mode propagates to that uninstaller in ways CPack does not test; a
    // silent failure there would leave a half-uninstalled application. The visible welcome page is
    // also a natural pause while this process finishes exiting, and the user is prompted for
    // elevation eitherway, so silence buys nothing.
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info)) {
        const DWORD err = ::GetLastError();
        // A declined elevation prompt is a failure, not a success: treating it as success would
        // shut the application down with no installer running.
        if (err == ERROR_CANCELLED)
            error_out = "The update was cancelled at the Windows permission prompt.";
        else
            error_out = (boost::format("Could not start the installer (Windows error %1%).") % err).str();
        BOOST_LOG_TRIVIAL(error) << "updater: ShellExecuteExW failed: " << error_out;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "updater: launched " << installer.string();
    return true;
#else
    (void) installer;
    error_out = "Installing an update from inside the application is not supported on this platform.";
    return false;
#endif
}

void cleanup_download_dir(const fs::path &installer)
{
    if (installer.empty())
        return;
    boost::system::error_code ec;
    const fs::path            dir = installer.parent_path();
    // Only ever remove a directory this module created.
    if (dir.filename().string().rfind("unlayered-update-", 0) == 0)
        fs::remove_all(dir, ec);
}

} // namespace Slic3r
