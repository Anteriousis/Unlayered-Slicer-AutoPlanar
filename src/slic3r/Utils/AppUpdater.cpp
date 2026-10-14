#include "AppUpdater.hpp"

#include <algorithm>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/platinfo.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/FirmwareBundle.hpp" // sha256_hex
#include "slic3r/Utils/Http.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
    #include <tlhelp32.h>
#endif

#ifdef __APPLE__
    #include <mach-o/dyld.h>
    #include <spawn.h>
    #include <unistd.h>
    extern char **environ;
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

#ifdef __APPLE__
// The running executable, and the .app bundle that contains it.
fs::path current_exe_path()
{
    uint32_t    size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    buf.resize(strlen(buf.c_str()));

    boost::system::error_code ec;
    const fs::path            resolved = fs::canonical(fs::path(buf), ec);
    return ec ? fs::path(buf) : resolved;
}

// .../Foo.app/Contents/MacOS/Foo -> .../Foo.app, or empty when not inside a bundle.
fs::path enclosing_app_bundle(const fs::path &exe)
{
    for (fs::path dir = exe.parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
        if (boost::algorithm::iends_with(dir.filename().string(), ".app"))
            return dir;
    }
    return {};
}

// Shell single-quoting: close, escape, reopen. Covers spaces, which install paths have.
std::string sh_quote(const std::string &in)
{
    std::string out = "'";
    for (char c : in)
        c == '\'' ? out += "'\\''" : out += c;
    out += "'";
    return out;
}
#endif

#ifdef _WIN32
fs::path current_exe_path()
{
    std::wstring buf(32768, L'\0');
    const DWORD  n = ::GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize(n);
    return fs::path(buf);
}

bool same_path(const fs::path &a, const fs::path &b)
{
    boost::system::error_code ec;
    if (fs::equivalent(a, b, ec))
        return true;
    return boost::algorithm::iequals(a.lexically_normal().wstring(), b.lexically_normal().wstring());
}

// Must match CPACK_PACKAGE_INSTALL_REGISTRY_KEY and CPACK_NSIS_UNINSTALL_NAME in CMakeLists.txt.
constexpr const wchar_t *UNINSTALL_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Unlayered Slicer";
constexpr const char    *UNINSTALL_NAME = "Uninstall.exe";

std::wstring read_reg_string(HKEY root, const wchar_t *key, const wchar_t *value)
{
    HKEY h = nullptr;
    if (::RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return {};
    std::wstring out;
    DWORD        type = 0, size = 0;
    if (::RegQueryValueExW(h, value, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ && size > 0) {
        out.resize(size / sizeof(wchar_t));
        if (::RegQueryValueExW(h, value, nullptr, nullptr, reinterpret_cast<LPBYTE>(out.data()), &size) == ERROR_SUCCESS) {
            while (!out.empty() && out.back() == L'\0')
                out.pop_back();
        } else
            out.clear();
    }
    ::RegCloseKey(h);
    return out;
}

std::wstring read_hkcu_string(const wchar_t *key, const wchar_t *value)
{
    return read_reg_string(HKEY_CURRENT_USER, key, value);
}

// An UninstallString's folder, without the quotes some CMake versions add.
boost::filesystem::path uninstall_dir(std::wstring uninstall)
{
    if (uninstall.size() >= 2 && uninstall.front() == L'"' && uninstall.back() == L'"')
        uninstall = uninstall.substr(1, uninstall.size() - 2);
    return uninstall.empty() ? boost::filesystem::path() : boost::filesystem::path(uninstall).parent_path();
}
#endif

} // namespace

const char *updater_platform()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    return "linux";
#endif
}

const char *updater_arch() { return detect_updater_arch() == "arm64" ? "arm64" : "x64"; }

namespace {

// An asset file that installs this platform's build (any architecture).
bool asset_is_for_this_platform(const std::string &file_name)
{
    const std::string n = boost::algorithm::to_lower_copy(file_name);
    // Published alongside the installers; none of them is the application.
    if (n.find("validator") != std::string::npos || n.find("pdb") != std::string::npos)
        return false;
#if defined(_WIN32)
    return boost::algorithm::iends_with(n, ".exe") || boost::algorithm::iends_with(n, ".msi");
#elif defined(__APPLE__)
    return boost::algorithm::iends_with(n, ".dmg");
#else
    return boost::algorithm::iends_with(n, ".appimage");
#endif
}

} // namespace

bool release_targets_platform(const std::vector<AppUpdateAsset> &assets, const std::vector<AccountDownload> &downloads)
{
    if (assets.empty() && downloads.empty())
        return true;
    for (const auto &d : downloads)
        if (boost::algorithm::iequals(d.platform, updater_platform()))
            return true;
    for (const auto &a : assets)
        if (asset_is_for_this_platform(a.name))
            return true;
    return false;
}

std::optional<AccountDownload> select_account_download(const std::vector<AccountDownload> &downloads)
{
#if defined(_WIN32)
    for (const auto &d : downloads) {
        if (!boost::algorithm::iequals(d.platform, updater_platform()) || !boost::algorithm::iequals(d.arch, updater_arch()))
            continue;
        if (d.url.empty() || d.sha256.size() != 64)
            continue;
        return d;
    }
    return std::nullopt;
#elif defined(__APPLE__)
    for (const auto &d : downloads) {
        if (!boost::algorithm::iequals(d.platform, updater_platform()))
            continue;
        // The macOS build ships as one universal .dmg, so a feed entry may name the
        // architecture or say "universal"; both are installable here. An entry naming the
        // other architecture is not.
        if (!d.arch.empty() && !boost::algorithm::iequals(d.arch, updater_arch()) &&
            !boost::algorithm::iequals(d.arch, "universal"))
            continue;
        if (d.url.empty() || d.sha256.size() != 64)
            continue;
        return d;
    }
    return std::nullopt;
#else
    // Linux: an AppImage has to be made executable and swapped in place, possibly on a
    // read-only mount. Still the download page.
    (void) downloads;
    return std::nullopt;
#endif
}

std::string self_update_blocker()
{
#if defined(_WIN32)
    const fs::path exe = current_exe_path();
    if (exe.empty())
        return "unknown";
    const fs::path dir = exe.parent_path();
    boost::system::error_code ec;
    if (!fs::exists(dir / UNINSTALL_NAME, ec))
        return "no_uninstaller";
    const fs::path user_dir = uninstall_dir(read_hkcu_string(UNINSTALL_KEY, L"UninstallString"));
    if (!user_dir.empty() && same_path(user_dir, dir))
        return {};
    const fs::path machine_dir = uninstall_dir(read_reg_string(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"UninstallString"));
    if (!machine_dir.empty() && same_path(machine_dir, dir))
        return "all_users";
    return user_dir.empty() ? "not_registered" : "registered_elsewhere";
#elif defined(__APPLE__)
    return self_updatable_install_dir() ? std::string() : std::string("not_in_applications");
#else
    return "unsupported";
#endif
}

std::optional<fs::path> self_updatable_install_dir()
{
#if defined(_WIN32)
    const fs::path exe = current_exe_path();
    if (exe.empty())
        return std::nullopt;
    const fs::path dir = exe.parent_path();

    boost::system::error_code ec;
    if (!fs::exists(dir / UNINSTALL_NAME, ec))
        return std::nullopt; // portable copy or a development build: never overwrite it

    // The per-user installer registers under HKCU. A per-machine install registers under HKLM, lives
    // in Program Files and needs elevation to replace, so it is deliberately not matched here.
    std::wstring uninstall = read_hkcu_string(UNINSTALL_KEY, L"UninstallString");
    if (uninstall.size() >= 2 && uninstall.front() == L'"' && uninstall.back() == L'"')
        uninstall = uninstall.substr(1, uninstall.size() - 2);
    if (uninstall.empty() || !same_path(fs::path(uninstall).parent_path(), dir))
        return std::nullopt;

    return dir;
#elif defined(__APPLE__)
    // The bundle itself is what gets replaced, so this returns the .app rather than a parent
    // directory. Refuse anything we could not swap: a build tree, or a copy still running from
    // a read-only mount such as the installer image.
    const fs::path exe = current_exe_path();
    if (exe.empty())
        return std::nullopt;
    const fs::path app = enclosing_app_bundle(exe);
    if (app.empty())
        return std::nullopt;

    boost::system::error_code ec;
    if (!fs::exists(app / "Contents" / "Info.plist", ec))
        return std::nullopt;

    // Only a real installation updates itself. macOS has no install registry to consult the way
    // the Windows path does, so the test is where the bundle lives: an application directory.
    // That is what dragging the disk image to Applications produces, and it excludes the two
    // cases Windows excludes by other means -- a development build run straight from the build
    // tree, and a copy running from the mounted installer image.
    const fs::path parent = app.parent_path();
    const auto     is_dir = [&parent](const fs::path &candidate) {
        boost::system::error_code e;
        if (fs::equivalent(parent, candidate, e)) // follows symlinks; false (not throw) if absent
            return true;
        return parent.lexically_normal() == candidate.lexically_normal();
    };
    const char *home           = ::getenv("HOME");
    const bool  in_applications = is_dir(fs::path("/Applications")) ||
                                 (home != nullptr && is_dir(fs::path(home) / "Applications"));
    if (!in_applications)
        return std::nullopt;

    // Writable by this user without authorisation; an install owned by another admin, or one on
    // a read-only volume, falls back to the download page.
    if (::access(app.c_str(), W_OK) != 0 || ::access(parent.c_str(), W_OK) != 0)
        return std::nullopt;

    return app;
#else
    return std::nullopt;
#endif
}

bool other_instances_running()
{
#if defined(_WIN32)
    const fs::path exe = current_exe_path();
    if (exe.empty())
        return false;
    const DWORD self = ::GetCurrentProcessId();

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    bool           found = false;
    PROCESSENTRY32W pe   = {};
    pe.dwSize            = sizeof(pe);
    for (BOOL more = ::Process32FirstW(snap, &pe); more && !found; more = ::Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || _wcsicmp(pe.szExeFile, exe.filename().c_str()) != 0)
            continue;
        // Same file name; only the same FILE counts. A process this account cannot inspect belongs
        // to another user, whose per-user install is a different folder.
        HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!proc)
            continue;
        std::wstring buf(32768, L'\0');
        DWORD        len = DWORD(buf.size());
        if (::QueryFullProcessImageNameW(proc, 0, buf.data(), &len)) {
            buf.resize(len);
            found = same_path(fs::path(buf), exe);
        }
        ::CloseHandle(proc);
    }
    ::CloseHandle(snap);
    return found;
#else
    return false;
#endif
}

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
#elif defined(__APPLE__)
    const std::string arch = updater_arch(); // "arm64" or "x64"
    for (const auto &a : assets) {
        const std::string n = boost::algorithm::to_lower_copy(a.name);
        if (!boost::algorithm::iends_with(n, ".dmg"))
            continue;
        // Published alongside the installer; none of them is the application.
        if (n.find("validator") != std::string::npos || n.find("pdb") != std::string::npos ||
            n.find("portable") != std::string::npos)
            continue;
        // One universal .dmg covers both architectures. Accept it, or an image that names this
        // architecture, but never one that names only the other.
        const bool universal = n.find("universal") != std::string::npos;
        const bool this_arch = n.find(arch) != std::string::npos ||
                               (arch == "x64" && n.find("x86_64") != std::string::npos);
        const bool other_arch = !universal && !this_arch &&
                                (n.find("arm64") != std::string::npos || n.find("x86_64") != std::string::npos ||
                                 n.find("x64") != std::string::npos);
        if (other_arch)
            continue;
        return a;
    }
    return std::nullopt;
#else
    // Linux falls back to the download page: an AppImage has to be made executable and swapped
    // in place, possibly on a read-only mount.
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

    // The installer host drops long transfers part-way (Vercel Blob cut a 190 MB download at about
    // 66 s, 131 MB in, every time on a 2 MB/s link), so a dropped connection resumes with a Range
    // request instead of failing. Only a network error resumes: an HTTP error status is final.
    constexpr int max_attempts = 8;
    for (int attempt = 1; attempt <= max_attempts && !ok && !cancelled; ++attempt) {
        const size_t offset    = payload.size();
        bool         dropped   = false;
        bool         partial   = false; // the server honoured the Range request
        std::string  headers;

        auto http = Http::get(asset.url);
        // Verified TLS: this file is about to be executed. The shared client defaults verification off
        // for self-signed print hosts, which must never apply to an installer.
        http.tls_verify(true)
            .timeout_connect(15)
            // 0 means "no cap". A large download on a slow link is not an error, and cancellation is
            // driven by on_progress rather than a wall clock.
            .timeout_max(0)
            .size_limit(1024ull * 1024 * 1024)
            .header("User-Agent", updater_user_agent())
            .header("Accept", "application/octet-stream");
        if (offset > 0)
            http.header("Range", "bytes=" + std::to_string(offset) + "-");
        http.on_header_callback([&](std::string h) {
                headers = std::move(h);
                partial = offset > 0 && boost::algorithm::icontains(headers, "\ncontent-range:");
            })
            .on_progress([&](Http::Progress p, bool &cancel) {
                const size_t base = partial ? offset : 0;
                if (progress && !progress(base + p.dlnow, p.dltotal ? base + p.dltotal : asset.size)) {
                    cancel    = true;
                    cancelled = true;
                    error_out = "Cancelled";
                }
            })
            .on_error([&](std::string body, std::string err, unsigned status) {
                if (cancelled)
                    return;
                error_out = err.empty() ? (boost::format("HTTP %1%") % status).str() : err;
                // status 0 is curl's own failure (the connection dropped), not an answer from the
                // server. Keep what arrived and ask for the rest.
                if (status == 0 && !body.empty()) {
                    if (partial)
                        payload += body;
                    else
                        payload = std::move(body);
                    dropped = true;
                }
            })
            .on_complete([&](std::string body, unsigned status) {
                // A 200 to a Range request is the whole file again: start over from it.
                if (offset > 0 && status == 206)
                    payload += body;
                else
                    payload = std::move(body);
                ok = true;
            })
            .perform_sync();

        if (!ok && !dropped)
            break;
        if (!ok)
            BOOST_LOG_TRIVIAL(warning) << "updater: download dropped at " << payload.size() << " of " << asset.size
                                       << " bytes (" << error_out << "); resuming, attempt " << attempt + 1;
    }

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
    // The default verb, not "runas": the installer's manifest says what it needs (nothing for the
    // per-user build, administrator for the per-machine one) and the shell prompts accordingly.
    info.lpVerb = nullptr;
    info.lpFile = file.c_str();
    // Interactive rather than /S. The installer removes the previous version before installing the
    // new one, and NSIS silent mode propagates to that uninstaller in ways CPack does not test; a
    // silent failure there would leave a half-uninstalled application. The visible welcome page is
    // also a natural pause while this process finishes exiting. This is the path for installs that
    // cannot update themselves (per-machine, or not signed in); launch_silent_update() is the
    // other one.
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

bool launch_silent_update(const fs::path &installer, const fs::path &install_dir, bool relaunch, std::string &error_out)
{
#if defined(_WIN32)
    const std::wstring file = installer.wstring();
    // /UPDATE, /WAITPID and /RELAUNCH are ours (cmake/modules/NSIS.template.in). /D must come last
    // and must NOT be quoted, even with spaces in it - that is NSIS's rule, not a bug here.
    std::wstring params = L"/S /UPDATE /WAITPID=" + std::to_wstring(::GetCurrentProcessId());
    if (relaunch)
        params += L" /RELAUNCH";
    params += L" /D=" + install_dir.wstring();

    SHELLEXECUTEINFOW info = {};
    info.cbSize            = sizeof(info);
    info.fMask             = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb            = nullptr; // the per-user installer asks for no elevation
    info.lpFile            = file.c_str();
    info.lpParameters      = params.c_str();
    info.nShow             = SW_HIDE;

    if (!::ShellExecuteExW(&info)) {
        error_out = (boost::format("Could not start the installer (Windows error %1%).") % ::GetLastError()).str();
        BOOST_LOG_TRIVIAL(error) << "updater: " << error_out;
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "updater: silent update started: " << installer.string() << " "
                            << boost::nowide::narrow(params);
    return true;
#elif defined(__APPLE__)
    // A running .app cannot replace itself, so hand the job to a small detached script that
    // waits for this process to go away first. Same shape as the Windows path, where the NSIS
    // installer waits on /WAITPID.
    //
    // The copy is staged beside the target and swapped in, so a failure part-way leaves the
    // installed application untouched rather than half-written. ditto is used rather than cp
    // because it preserves the extended attributes and the code signature of the bundle.
    const fs::path app = install_dir; // the .app itself; see self_updatable_install_dir()
    if (app.empty() || !boost::algorithm::iends_with(app.filename().string(), ".app")) {
        error_out = "The installed application could not be located.";
        return false;
    }

    const fs::path script = installer.parent_path() / "install-update.sh";
    const std::string mount = (installer.parent_path() / "mnt").string();

    std::string sh;
    sh += "#!/bin/sh\n";
    sh += "set -e\n";
    // Wait for the slicer to exit; give up rather than hang forever if it never does.
    sh += "for i in $(seq 1 600); do kill -0 " + std::to_string(get_current_pid()) +
          " 2>/dev/null || break; sleep 0.5; done\n";
    sh += "MNT=" + sh_quote(mount) + "\n";
    sh += "APP=" + sh_quote(app.string()) + "\n";
    sh += "DMG=" + sh_quote(installer.string()) + "\n";
    sh += "mkdir -p \"$MNT\"\n";
    sh += "hdiutil attach \"$DMG\" -nobrowse -noautoopen -mountpoint \"$MNT\" >/dev/null\n";
    // Undo the mount whatever happens next, so a failed update leaves no attached image behind.
    sh += "trap 'hdiutil detach \"$MNT\" -quiet >/dev/null 2>&1 || true' EXIT\n";
    sh += "SRC=$(find \"$MNT\" -maxdepth 1 -name '*.app' -print -quit)\n";
    sh += "[ -n \"$SRC\" ] || exit 1\n";
    sh += "NEW=\"$APP.update-staging\"\n";
    sh += "rm -rf \"$NEW\"\n";
    sh += "ditto \"$SRC\" \"$NEW\"\n";
    // Swap: move the old aside, put the new in place, then delete the old. The window in which
    // neither exists is one rename long.
    sh += "rm -rf \"$APP.update-old\"\n";
    sh += "mv \"$APP\" \"$APP.update-old\"\n";
    sh += "mv \"$NEW\" \"$APP\" || { mv \"$APP.update-old\" \"$APP\"; exit 1; }\n";
    sh += "rm -rf \"$APP.update-old\"\n";
    if (relaunch)
        sh += "open \"$APP\"\n";
    // The download directory holds the image, this script and the mount point.
    sh += "rm -rf " + sh_quote(installer.parent_path().string()) + " >/dev/null 2>&1 || true\n";

    {
        boost::filesystem::ofstream f(script);
        if (!f) {
            error_out = "Could not write the update script.";
            return false;
        }
        f << sh;
    }
    boost::system::error_code ec;
    fs::permissions(script, fs::owner_all, ec);

    // Detached, so it outlives this process -- which is the whole point.
    const std::string script_path = script.string();
    char *const       argv[]      = {const_cast<char *>("/bin/sh"), const_cast<char *>(script_path.c_str()), nullptr};
    pid_t             pid         = 0;
    if (::posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv, environ) != 0) {
        error_out = "Could not start the update script.";
        BOOST_LOG_TRIVIAL(error) << "updater: " << error_out;
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "updater: update script started (pid " << pid << ") for " << app.string();
    return true;
#else
    (void) installer;
    (void) install_dir;
    (void) relaunch;
    error_out = "Installing an update from inside the application is not supported on this platform.";
    return false;
#endif
}

void cleanup_stale_download_dirs()
{
    boost::system::error_code ec;
    const fs::path            tmp  = fs::temp_directory_path(ec);
    if (ec)
        return;
    const std::string mine = std::string("unlayered-update-") + std::to_string(get_current_pid());
    for (fs::directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("unlayered-update-", 0) != 0 || name == mine)
            continue;
        // Fails harmlessly while an installer from that directory is still running.
        boost::system::error_code rm_ec;
        fs::remove_all(it->path(), rm_ec);
    }
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
