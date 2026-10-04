#include "FirmwareBundle.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/evp.h>

#include "libslic3r/Utils.hpp"
#include "DuetClient.hpp"
#include "Http.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

// -----------------------------------------------------------------------------------------------
// Display identity
// -----------------------------------------------------------------------------------------------

DisplayInfo detect_display(DuetClient &client)
{
    DisplayInfo info;

    pt::ptree   globals;
    std::string err;
    if (!client.object_model("global", "v", globals, err)) {
        // Not an error: a board with no globals at all answers this way too.
        info.evidence = "the printer did not report any global variables";
        BOOST_LOG_TRIVIAL(info) << "Firmware: display detection could not read globals: " << err;
        return info;
    }

    auto role_for = [](int id) {
        switch (id) {
        case 1:  return FirmwareRole::PanelDue;
        case 2:  return FirmwareRole::PanelOta;
        default: return FirmwareRole::Unknown;
        }
    };

    // 1. What the display itself announced this boot. Highest authority, but absent on any machine
    //    whose display has not called displayid.g since the last restart.
    if (const auto id = globals.get_optional<int>("displayId")) {
        const FirmwareRole role = role_for(*id);
        if (role != FirmwareRole::Unknown) {
            info.role     = role;
            info.certain  = true;
            info.evidence = (boost::format("the display identified itself (global.displayId = %1%)") % *id).str();
            return info;
        }
        BOOST_LOG_TRIVIAL(warning) << "Firmware: unrecognised global.displayId = " << *id
                                   << "; falling back to global.displayType";
    }

    // 2. What the operator confirmed during commissioning, persisted in 0:/sys/display.g and run
    //    by config.g at every boot. Present on every commissioned machine, and the only source
    //    that can state there is no display at all.
    if (const auto type = globals.get_optional<int>("displayType")) {
        if (*type == 0) {
            info.none     = true;
            info.evidence = "this printer is set up as having no display (global.displayType = 0)";
            return info;
        }
        const FirmwareRole role = role_for(*type);
        if (role != FirmwareRole::Unknown) {
            info.role     = role;
            info.certain  = true;
            info.evidence = (boost::format("set up with this display (global.displayType = %1%)") % *type).str();
            return info;
        }
        info.evidence = (boost::format("the printer reports global.displayType = %1%, which is not a "
                                       "display this version knows about") % *type).str();
        return info;
    }

    info.evidence = "this printer reports neither global.displayId nor global.displayType, so it "
                    "cannot say which display it has";
    return info;
}

// -----------------------------------------------------------------------------------------------
// Roles
// -----------------------------------------------------------------------------------------------

int firmware_role_to_module(FirmwareRole role)
{
    switch (role) {
    case FirmwareRole::Mainboard: return 0;
    case FirmwareRole::Wifi:      return 1;
    case FirmwareRole::PanelDue:  return 4;
    case FirmwareRole::PanelOta:  return 5;
    // The IAP is the programmer that flashes the mainboard; it is never an M997 target itself.
    case FirmwareRole::Iap:
    case FirmwareRole::Unknown:
    default:                      return -1;
    }
}

FirmwareRole firmware_role_from_string(const std::string &s)
{
    const std::string v = boost::algorithm::to_lower_copy(s);
    if (v == "mainboard" || v == "main" || v == "firmware") return FirmwareRole::Mainboard;
    if (v == "iap")                                         return FirmwareRole::Iap;
    if (v == "wifi" || v == "wifiserver")                   return FirmwareRole::Wifi;
    if (v == "paneldue" || v == "display")                  return FirmwareRole::PanelDue;
    // "display" stays on PanelDue: it is what manifests written before the ESP32 panel existed
    // meant by it, and silently re-pointing those at module 5 would flash the wrong device.
    if (v == "panelota" || v == "panel" || v == "esp32panel") return FirmwareRole::PanelOta;
    return FirmwareRole::Unknown;
}

const char *firmware_role_to_string(FirmwareRole role)
{
    switch (role) {
    case FirmwareRole::Mainboard: return "mainboard";
    case FirmwareRole::Iap:       return "iap";
    case FirmwareRole::Wifi:      return "wifi";
    case FirmwareRole::PanelDue:  return "paneldue";
    case FirmwareRole::PanelOta:  return "panelota";
    default:                      return "unknown";
    }
}

// -----------------------------------------------------------------------------------------------
// FirmwarePackage
// -----------------------------------------------------------------------------------------------

const FirmwareImage *FirmwarePackage::find(FirmwareRole role) const
{
    auto it = std::find_if(images.begin(), images.end(), [role](const FirmwareImage &i) { return i.role == role; });
    return it == images.end() ? nullptr : &*it;
}

FirmwareImage *FirmwarePackage::find(FirmwareRole role)
{
    auto it = std::find_if(images.begin(), images.end(), [role](const FirmwareImage &i) { return i.role == role; });
    return it == images.end() ? nullptr : &*it;
}

uint64_t FirmwarePackage::total_bytes() const
{
    uint64_t n = 0;
    for (const auto &i : images)
        n += i.data.size();
    return n;
}

// -----------------------------------------------------------------------------------------------
// Validation helpers
// -----------------------------------------------------------------------------------------------

bool is_uf2_image(const std::vector<uint8_t> &data)
{
    // UF2 blocks begin with the two magic words 0x0A324655 ("UF2\n") and 0x9E5D5157, little endian.
    if (data.size() < 8)
        return false;
    auto rd32 = [&data](size_t off) {
        return uint32_t(data[off]) | (uint32_t(data[off + 1]) << 8) | (uint32_t(data[off + 2]) << 16) |
               (uint32_t(data[off + 3]) << 24);
    };
    return rd32(0) == 0x0A324655u && rd32(4) == 0x9E5D5157u;
}

std::string sha256_hex(const std::vector<uint8_t> &data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  md_len = 0;
    if (::EVP_Digest(data.empty() ? reinterpret_cast<const unsigned char *>("") : data.data(),
                     data.size(), md, &md_len, ::EVP_sha256(), nullptr) != 1)
        return {};

    std::string out;
    out.reserve(md_len * 2);
    static const char *hex = "0123456789abcdef";
    for (unsigned int i = 0; i < md_len; ++i) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0x0F]);
    }
    return out;
}

bool validate_package(const FirmwarePackage &pkg, const std::string &board_short_name, std::string &error_out)
{
    if (pkg.images.empty()) {
        error_out = "The firmware package contains no images";
        return false;
    }

    // A package that declares a board must match the board we are talking to. This is the check
    // that stops a Mini 5+ image being pushed at a different Duet.
    if (!pkg.board_short.empty() && !board_short_name.empty() &&
        !boost::iequals(pkg.board_short, board_short_name)) {
        error_out = (boost::format("This firmware is for a %1%, but the printer reports a %2%")
                     % pkg.board_short % board_short_name).str();
        return false;
    }

    for (const auto &img : pkg.images) {
        if (!img.loaded()) {
            error_out = (boost::format("Firmware image %1% was not downloaded") % img.filename).str();
            return false;
        }
        if (!img.sha256.empty()) {
            const std::string actual = sha256_hex(img.data);
            if (!boost::iequals(actual, img.sha256)) {
                error_out = (boost::format("Checksum mismatch for %1%: expected %2%, got %3%")
                             % img.filename % img.sha256 % actual).str();
                return false;
            }
        }
        // Only the mainboard image is UF2 on the boards we support; the IAP and the WiFi module
        // firmware are raw binaries, so the magic check applies by extension rather than by role.
        if (boost::iends_with(img.filename, ".uf2") && !is_uf2_image(img.data)) {
            error_out = (boost::format("%1% does not look like a UF2 image; it may be corrupted or "
                                       "intended for a different board") % img.filename).str();
            return false;
        }
    }

    if (!pkg.find(FirmwareRole::Mainboard) && !pkg.find(FirmwareRole::Wifi) &&
        !pkg.find(FirmwareRole::PanelDue) && !pkg.find(FirmwareRole::PanelOta)) {
        error_out = "The firmware package contains nothing that can be flashed";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// Manifest
// -----------------------------------------------------------------------------------------------

bool parse_manifest(const std::string &json, FirmwarePackage &pkg_out, std::string &error_out)
{
    try {
        pt::ptree root;
        std::istringstream iss(json);
        pt::read_json(iss, root);

        pkg_out.version     = root.get<std::string>("version", "");
        pkg_out.build_date  = root.get<std::string>("buildDate", "");
        pkg_out.board_short = root.get<std::string>("board", "");
        pkg_out.notes       = root.get<std::string>("notes", "");

        if (pkg_out.version.empty()) {
            error_out = "The firmware manifest does not declare a version";
            return false;
        }

        auto images = root.get_child_optional("images");
        if (!images || images->empty()) {
            error_out = "The firmware manifest does not list any images";
            return false;
        }

        pkg_out.images.clear();
        for (const auto &kv : *images) {
            FirmwareImage img;
            img.role     = firmware_role_from_string(kv.second.get<std::string>("role", ""));
            img.filename = kv.second.get<std::string>("file", "");
            img.sha256   = boost::algorithm::to_lower_copy(kv.second.get<std::string>("sha256", ""));
            if (img.filename.empty()) {
                error_out = "A firmware manifest entry has no file name";
                return false;
            }
            if (img.role == FirmwareRole::Unknown) {
                error_out = (boost::format("Firmware manifest entry %1% has an unrecognised role") % img.filename).str();
                return false;
            }
            pkg_out.images.push_back(std::move(img));
        }
        return true;
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the firmware manifest: %1%") % e.what()).str();
        return false;
    }
}

// Infers a role by comparing a file name with the names the board reported for itself. This is
// the same approach DuetWebControl takes and it is what lets an arbitrarily named local build be
// recognised. Matching is a case-insensitive stem comparison so "..._OLD.uf2" still matches.
static FirmwareRole infer_role_from_name(const std::string &filename,
                                         const std::string &board_firmware,
                                         const std::string &board_iap,
                                         const std::string &board_wifi)
{
    auto stem_of = [](const std::string &s) {
        const auto dot = s.find_last_of('.');
        return boost::algorithm::to_lower_copy(dot == std::string::npos ? s : s.substr(0, dot));
    };
    const std::string stem = stem_of(filename);

    // Longest-stem-first so that a board firmware name that happens to be a prefix of another
    // does not win by accident.
    struct Candidate { std::string stem; FirmwareRole role; };
    std::vector<Candidate> candidates;
    if (!board_iap.empty())      candidates.push_back({stem_of(board_iap), FirmwareRole::Iap});
    if (!board_firmware.empty()) candidates.push_back({stem_of(board_firmware), FirmwareRole::Mainboard});
    if (!board_wifi.empty())     candidates.push_back({stem_of(board_wifi), FirmwareRole::Wifi});
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.stem.size() > b.stem.size(); });

    for (const auto &c : candidates)
        if (!c.stem.empty() && stem.rfind(c.stem, 0) == 0)
            return c.role;

    // Fall back to the well-known names for hardware the object model does not describe.
    if (stem.rfind("duetwifiserver", 0) == 0 || stem.rfind("duetwifimodule", 0) == 0 ||
        stem.rfind("duetwifisocketserver", 0) == 0)
        return FirmwareRole::Wifi;
    if (stem.rfind("paneldue", 0) == 0 || stem.rfind("duetscreen", 0) == 0)
        return FirmwareRole::PanelDue;
    if (stem.find("iap") != std::string::npos)
        return FirmwareRole::Iap;

    return FirmwareRole::Unknown;
}

// -----------------------------------------------------------------------------------------------
// BundledFirmwareSource
// -----------------------------------------------------------------------------------------------

BundledFirmwareSource::BundledFirmwareSource()
    : m_directory((fs::path(resources_dir()) / "firmware").string())
{}

BundledFirmwareSource::BundledFirmwareSource(std::string directory) : m_directory(std::move(directory)) {}

bool BundledFirmwareSource::available() const
{
    boost::system::error_code ec;
    return fs::exists(fs::path(m_directory) / "manifest.json", ec);
}

std::string BundledFirmwareSource::description() const
{
    return (boost::format("Firmware bundled with the application (%1%)") % m_directory).str();
}

bool BundledFirmwareSource::fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out)
{
    const fs::path dir      = m_directory;
    const fs::path manifest = dir / "manifest.json";

    boost::system::error_code ec;
    if (!fs::exists(manifest, ec)) {
        error_out = (boost::format("No firmware is bundled with this installation (expected %1%)") % manifest.string()).str();
        return false;
    }

    std::ifstream f(manifest.string());
    if (!f) {
        error_out = (boost::format("Could not open %1%") % manifest.string()).str();
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (!parse_manifest(json, pkg_out, error_out))
        return false;

    pkg_out.origin = description();

    uint64_t done = 0;
    for (auto &img : pkg_out.images) {
        const fs::path p = dir / img.filename;
        std::string    err;
        if (!DuetClient::read_file_binary(p.string(), img.data, err)) {
            error_out = (boost::format("Bundled firmware image is missing: %1%") % p.string()).str();
            return false;
        }
        done += img.data.size();
        if (progress && !progress("Reading bundled firmware", done, done)) {
            error_out = "Cancelled";
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// LocalFileFirmwareSource
// -----------------------------------------------------------------------------------------------

LocalFileFirmwareSource::LocalFileFirmwareSource(std::string path,
                                                 std::string board_firmware_filename,
                                                 std::string board_iap_filename,
                                                 std::string board_wifi_filename)
    : m_path(std::move(path))
    , m_board_firmware_filename(std::move(board_firmware_filename))
    , m_board_iap_filename(std::move(board_iap_filename))
    , m_board_wifi_filename(std::move(board_wifi_filename))
{}

std::string LocalFileFirmwareSource::description() const
{
    return (boost::format("Local file (%1%)") % m_path).str();
}

bool LocalFileFirmwareSource::fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out)
{
    const fs::path p = m_path;
    FirmwareImage  img;
    if (!DuetClient::read_file_binary(m_path, img.data, error_out))
        return false;

    const std::string given_name = p.filename().string();
    img.role = infer_role_from_name(given_name, m_board_firmware_filename, m_board_iap_filename, m_board_wifi_filename);

    if (img.role == FirmwareRole::Unknown) {
        // Last resort: a UF2 can only be the mainboard image on the boards we support.
        if (is_uf2_image(img.data) && !m_board_firmware_filename.empty())
            img.role = FirmwareRole::Mainboard;
        else {
            error_out = (boost::format("Could not tell what %1% is for. Expected a file named like %2% or %3%.")
                         % given_name
                         % (m_board_firmware_filename.empty() ? "the board firmware" : m_board_firmware_filename)
                         % (m_board_iap_filename.empty() ? "the IAP binary" : m_board_iap_filename)).str();
            return false;
        }
    }

    // Always upload under the name the board expects, whatever the file was called on disk.
    switch (img.role) {
    case FirmwareRole::Mainboard: img.filename = m_board_firmware_filename; break;
    case FirmwareRole::Iap:       img.filename = m_board_iap_filename;      break;
    case FirmwareRole::Wifi:      img.filename = m_board_wifi_filename;     break;
    default:                      img.filename = given_name;                break;
    }
    if (img.filename.empty())
        img.filename = given_name;

    pkg_out          = FirmwarePackage();
    pkg_out.origin   = description();
    // A hand-picked file carries no trustworthy version; the caller treats an empty version as
    // "unknown" and always offers to flash it rather than claiming it is up to date.
    pkg_out.version  = "";
    pkg_out.notes    = (boost::format("Selected file: %1%") % given_name).str();
    pkg_out.images.push_back(std::move(img));

    if (progress && !progress("Reading file", pkg_out.total_bytes(), pkg_out.total_bytes())) {
        error_out = "Cancelled";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// GitHubReleaseFirmwareSource
// -----------------------------------------------------------------------------------------------

GitHubReleaseFirmwareSource::GitHubReleaseFirmwareSource(Options opts) : m_opts(std::move(opts)) {}

std::string GitHubReleaseFirmwareSource::description() const
{
    return (boost::format("GitHub release %1%/%2% (%3%)")
            % m_opts.owner % m_opts.repo % (m_opts.tag.empty() ? std::string("latest") : m_opts.tag)).str();
}

bool GitHubReleaseFirmwareSource::fetch_metadata(FirmwarePackage &pkg_out, std::string &error_out)
{
    // /releases/latest deliberately skips pre-releases, and the Unlayered firmware line is
    // currently all -beta, so list releases and pick the newest usable one instead.
    std::string url;
    if (!m_opts.tag.empty())
        url = (boost::format("%1%/repos/%2%/%3%/releases/tags/%4%") % m_opts.api_base
               % m_opts.owner % m_opts.repo % Http::url_encode(m_opts.tag)).str();
    else if (m_opts.allow_prerelease)
        url = (boost::format("%1%/repos/%2%/%3%/releases?per_page=20") % m_opts.api_base
               % m_opts.owner % m_opts.repo).str();
    else
        url = (boost::format("%1%/repos/%2%/%3%/releases/latest") % m_opts.api_base
               % m_opts.owner % m_opts.repo).str();

    std::string body;
    bool        ok   = false;
    auto        http = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(30)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        // GitHub rejects API requests that do not identify themselves.
        .header("User-Agent", "UnlayeredSlicer");
    if (!m_opts.token.empty())
        http.header("Authorization", std::string("Bearer ") + m_opts.token);

    http.on_error([&](std::string b, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("Firmware: GitHub request failed: %1%, HTTP %2%") % error % status;
            if (status == 404)
                error_out = (boost::format("No firmware releases found at %1%/%2%") % m_opts.owner % m_opts.repo).str();
            else if (status == 403)
                error_out = "GitHub rejected the request (rate limited, or the repository is private)";
            else
                error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string b, unsigned) { body = std::move(b); ok = true; })
        .perform_sync();

    if (!ok)
        return false;

    try {
        pt::ptree root;
        std::istringstream iss(body);
        pt::read_json(iss, root);

        // Normalise: a tag/latest query returns one object, a list query returns an array.
        const pt::ptree *release = nullptr;
        pt::ptree        picked;
        if (m_opts.tag.empty() && m_opts.allow_prerelease) {
            for (const auto &kv : root) {
                if (kv.second.get<bool>("draft", false))
                    continue;
                picked  = kv.second;
                release = &picked;
                break;
            }
            if (!release) {
                error_out = (boost::format("No published firmware releases at %1%/%2%") % m_opts.owner % m_opts.repo).str();
                return false;
            }
        } else {
            release = &root;
        }

        const std::string tag = release->get<std::string>("tag_name", "");
        pkg_out.notes  = release->get<std::string>("body", "");
        pkg_out.origin = (boost::format("GitHub release %1% (%2%/%3%)") % tag % m_opts.owner % m_opts.repo).str();
        // Only a manifest can state the exact firmware version string; a tag is a reasonable
        // stand-in but is not authoritative, so it is overwritten below when a manifest exists.
        pkg_out.version    = boost::algorithm::starts_with(tag, "v") ? tag.substr(1) : tag;
        pkg_out.board_short = m_opts.board_short_name;

        m_assets.clear();
        if (auto assets = release->get_child_optional("assets")) {
            for (const auto &kv : *assets) {
                const std::string name = kv.second.get<std::string>("name", "");
                const std::string durl = kv.second.get<std::string>("browser_download_url", "");
                if (!name.empty() && !durl.empty())
                    m_assets.emplace_back(name, durl);
            }
        }
        if (m_assets.empty()) {
            error_out = (boost::format("Release %1% has no downloadable files attached") % tag).str();
            return false;
        }
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the GitHub release response: %1%") % e.what()).str();
        return false;
    }

    // The tag is only a stand-in. When the release carries a manifest, pull it now - it is a few
    // hundred bytes - so that the version shown to the user, and compared against the board, is
    // the authoritative one rather than whatever the tag happens to say.
    for (const auto &a : m_assets) {
        if (!boost::iequals(a.first, "manifest.json"))
            continue;

        std::vector<uint8_t> raw;
        std::string          ignored;
        if (!download_asset(a.second, "manifest.json", raw, nullptr, ignored)) {
            BOOST_LOG_TRIVIAL(warning) << "Firmware: could not read manifest.json from the release; "
                                          "falling back to the tag name for the version";
            break;
        }
        const std::string json(raw.begin(), raw.end());
        FirmwarePackage   from_manifest;
        if (!parse_manifest(json, from_manifest, ignored)) {
            BOOST_LOG_TRIVIAL(warning) << "Firmware: release manifest.json is malformed: " << ignored;
            break;
        }
        from_manifest.origin = pkg_out.origin;
        if (from_manifest.notes.empty())
            from_manifest.notes = pkg_out.notes;
        if (from_manifest.board_short.empty())
            from_manifest.board_short = m_opts.board_short_name;
        pkg_out = std::move(from_manifest);
        break;
    }

    return true;
}

bool GitHubReleaseFirmwareSource::download_asset(const std::string &url, const std::string &name,
                                                 std::vector<uint8_t> &out, FirmwareProgressFn progress,
                                                 std::string &error_out)
{
    bool ok        = false;
    bool cancelled = false;
    auto http      = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(300)
        .header("User-Agent", "UnlayeredSlicer")
        // Release assets are served as a redirect to a signed object URL; curl follows it.
        .header("Accept", "application/octet-stream");
    if (!m_opts.token.empty())
        http.header("Authorization", std::string("Bearer ") + m_opts.token);

    http.on_error([&](std::string b, std::string error, unsigned status) {
            if (!cancelled)
                error_out = (boost::format("Could not download %1%: %2%") % name
                             % (error.empty() ? (boost::format("HTTP %1%") % status).str() : error)).str();
        })
        .on_complete([&](std::string b, unsigned) {
            out.assign(b.begin(), b.end());
            ok = true;
        })
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && !progress((boost::format("Downloading %1%") % name).str(), p.dlnow, p.dltotal)) {
                cancel    = true;
                cancelled = true;
                error_out = "Cancelled";
            }
        })
        .perform_sync();

    return ok;
}

bool GitHubReleaseFirmwareSource::fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out)
{
    if (!fetch_metadata(pkg_out, error_out))
        return false;

    auto asset_url = [this](const std::string &name) -> std::string {
        for (const auto &a : m_assets)
            if (boost::iequals(a.first, name))
                return a.second;
        return {};
    };

    // fetch_metadata() has already applied the release manifest when there is one, so a populated
    // image list means the roles and checksums are known and we only need the payloads.
    if (!pkg_out.images.empty()) {
        for (auto &img : pkg_out.images) {
            const std::string url = asset_url(img.filename);
            if (url.empty()) {
                error_out = (boost::format("The release manifest lists %1%, but the release has no such file")
                             % img.filename).str();
                return false;
            }
            if (!download_asset(url, img.filename, img.data, progress, error_out))
                return false;
        }
        return true;
    }

    // Fallback: no manifest, so match asset names against what the board calls its own files.
    // This makes "attach the .uf2 and the IAP to a release" work with no extra ceremony.
    BOOST_LOG_TRIVIAL(info) << "Firmware: release has no manifest.json, matching assets by name";
    pkg_out.images.clear();
    for (const auto &a : m_assets) {
        const FirmwareRole role = infer_role_from_name(a.first,
                                                       m_opts.board_firmware_filename,
                                                       m_opts.board_iap_filename,
                                                       m_opts.board_wifi_filename);
        if (role == FirmwareRole::Unknown)
            continue;

        FirmwareImage img;
        img.role = role;
        switch (role) {
        case FirmwareRole::Mainboard: img.filename = m_opts.board_firmware_filename; break;
        case FirmwareRole::Iap:       img.filename = m_opts.board_iap_filename;      break;
        case FirmwareRole::Wifi:      img.filename = m_opts.board_wifi_filename;     break;
        default:                      img.filename = a.first;                        break;
        }
        if (img.filename.empty())
            img.filename = a.first;

        if (!download_asset(a.second, a.first, img.data, progress, error_out))
            return false;
        pkg_out.images.push_back(std::move(img));
    }

    if (pkg_out.images.empty()) {
        error_out = (boost::format("None of the files in that release look like firmware for a %1%")
                     % (m_opts.board_short_name.empty() ? "this board" : m_opts.board_short_name)).str();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// ChainedFirmwareSource
// -----------------------------------------------------------------------------------------------

void ChainedFirmwareSource::add(std::unique_ptr<FirmwareSource> source)
{
    if (source)
        m_sources.push_back(std::move(source));
}

std::string ChainedFirmwareSource::description() const
{
    if (!m_used.empty())
        return m_used;
    if (m_sources.empty())
        return "no firmware source";
    return m_sources.front()->description();
}

bool ChainedFirmwareSource::fetch(FirmwarePackage &pkg_out, FirmwareProgressFn progress, std::string &error_out)
{
    std::string first_error;
    for (auto &s : m_sources) {
        std::string err;
        if (s->fetch(pkg_out, progress, err)) {
            m_used = s->description();
            return true;
        }
        // A cancellation is the user's decision, not a source being unavailable: stop here rather
        // than silently falling through to the next one.
        if (err == "Cancelled") {
            error_out = err;
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "Firmware: source unavailable (" << s->description() << "): " << err;
        if (first_error.empty())
            first_error = err;
    }
    error_out = first_error.empty() ? "No firmware source is available" : first_error;
    return false;
}

bool ChainedFirmwareSource::fetch_metadata(FirmwarePackage &pkg_out, std::string &error_out)
{
    std::string first_error;
    for (auto &s : m_sources) {
        std::string err;
        if (s->fetch_metadata(pkg_out, err)) {
            m_used = s->description();
            return true;
        }
        BOOST_LOG_TRIVIAL(info) << "Firmware: source unavailable (" << s->description() << "): " << err;
        if (first_error.empty())
            first_error = err;
    }
    error_out = first_error.empty() ? "No firmware source is available" : first_error;
    return false;
}

} // namespace Slic3r
