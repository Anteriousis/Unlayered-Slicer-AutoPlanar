#include "SysFilesBundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "DuetRrfClient.hpp"
#include "FirmwareBundle.hpp" // sha256_hex
#include "Http.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

const char *SYS_MARKER_PATH = "sys/unlayered-sys.json";

// A configuration file is a few kilobytes; a height map is a few tens. These limits exist so that
// pointing the updater at the wrong zip fails immediately instead of pushing megabytes at a board
// that has to write them one FAT sector at a time.
static const uint64_t MAX_FILE_BYTES    = 4ull * 1024 * 1024;
static const uint64_t MAX_PACKAGE_BYTES = 32ull * 1024 * 1024;

const std::vector<std::string> &sys_allowed_roots()
{
    static const std::vector<std::string> roots = {"sys", "macros", "filaments"};
    return roots;
}

static bool is_allowed_root(const std::string &component)
{
    for (const auto &r : sys_allowed_roots())
        if (boost::iequals(r, component))
            return true;
    return false;
}

static std::string canonical_root(const std::string &component)
{
    for (const auto &r : sys_allowed_roots())
        if (boost::iequals(r, component))
            return r;
    return component;
}

// Separators, "./" and duplicate slashes only. Nothing here decides where a file goes.
static std::string normalize_separators(const std::string &in)
{
    std::string s = in;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (boost::starts_with(s, "./"))
        s = s.substr(2);
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/' && !out.empty() && out.back() == '/')
            continue;
        out.push_back(c);
    }
    return out;
}

static std::vector<std::string> split_path(const std::string &path)
{
    std::vector<std::string> parts;
    boost::split(parts, path, boost::is_any_of("/"));
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const std::string &p) { return p.empty() || p == "."; }),
                parts.end());
    return parts;
}

bool sys_map_path(const std::string &relative, std::string &card_path_out, std::string &reason_out)
{
    const std::string norm = normalize_separators(relative);

    if (norm.empty()) {
        reason_out = "the path is empty";
        return false;
    }
    // Anything that could resolve outside the card is refused outright rather than sanitised: a
    // package that contains such a path is wrong, and quietly rewriting it hides that.
    if (norm.front() == '/' || (norm.size() > 1 && norm[1] == ':')) {
        reason_out = "absolute paths are not allowed in a package";
        return false;
    }

    std::vector<std::string> parts = split_path(norm);
    if (parts.empty()) {
        reason_out = "the path is empty";
        return false;
    }
    for (const auto &p : parts) {
        if (p == "..") {
            reason_out = "the path leaves the package";
            return false;
        }
    }

    // A path already rooted at one of the destinations keeps it; anything else is configuration
    // for the machine, which is what 0:/sys is.
    if (!is_allowed_root(parts.front()))
        parts.insert(parts.begin(), "sys");
    parts.front() = canonical_root(parts.front());

    if (parts.size() < 2) {
        reason_out = "a package cannot write a directory itself";
        return false;
    }

    card_path_out = boost::join(parts, "/");
    return true;
}

bool sys_is_ignored_file(const std::string &relative_path)
{
    const std::string        norm  = normalize_separators(relative_path);
    const std::vector<std::string> parts = split_path(norm);
    if (parts.empty())
        return true;

    for (const auto &p : parts)
        if (boost::iequals(p, ".git") || boost::iequals(p, ".svn") || boost::iequals(p, "__MACOSX"))
            return true;

    const std::string name  = parts.back();
    const std::string lower = boost::algorithm::to_lower_copy(name);

    if (lower == "thumbs.db" || lower == ".ds_store" || lower == "desktop.ini")
        return true;
    if (!name.empty() && (name.front() == '~' || name.front() == '.'))
        return true;
    for (const char *ext : {".bak", ".part", ".tmp", ".old", ".orig", ".swp"})
        if (boost::iends_with(lower, ext))
            return true;

    // The marker is written by the installer itself; carrying one in from a card image would make
    // the printer claim to be running whatever that card last had.
    if (boost::iequals(norm, SYS_MARKER_PATH) || boost::iequals(name, "unlayered-sys.json"))
        return true;

    return false;
}

// -----------------------------------------------------------------------------------------------
// Package
// -----------------------------------------------------------------------------------------------

uint64_t SysFilePackage::total_bytes() const
{
    uint64_t n = 0;
    for (const auto &f : files)
        n += f.data.size();
    return n;
}

const SysFile *SysFilePackage::find(const std::string &path) const
{
    auto it = std::find_if(files.begin(), files.end(),
                           [&path](const SysFile &f) { return boost::iequals(f.path, path); });
    return it == files.end() ? nullptr : &*it;
}

bool parse_sys_manifest(const std::string &json, SysFilePackage &pkg_out, std::string &error_out)
{
    try {
        pt::ptree root;
        std::istringstream iss(json);
        pt::read_json(iss, root);

        pkg_out.version = root.get<std::string>("version", "");
        // "buildDate" is what the firmware manifest calls it; accept both so one generator can
        // produce either without a surprise.
        pkg_out.date    = root.get<std::string>("date", root.get<std::string>("buildDate", ""));
        pkg_out.machine = root.get<std::string>("machine", "");
        pkg_out.notes   = root.get<std::string>("notes", "");
        pkg_out.min_firmware = boost::algorithm::trim_copy(root.get<std::string>("minFirmware", ""));

        if (pkg_out.version.empty()) {
            error_out = "The manifest does not declare a version";
            return false;
        }

        pkg_out.files.clear();
        if (auto files = root.get_child_optional("files")) {
            for (const auto &kv : *files) {
                // Accept both { "path": "...", "sha256": "..." } and a bare "sys/config.g".
                const std::string declared = kv.second.get<std::string>("path", kv.second.data());
                if (declared.empty()) {
                    error_out = "A manifest entry has no path";
                    return false;
                }
                SysFile f;
                std::string reason;
                if (!sys_map_path(declared, f.path, reason)) {
                    error_out = (boost::format("The manifest lists %1%, which cannot be installed: %2%")
                                 % declared % reason).str();
                    return false;
                }
                f.sha256 = boost::algorithm::to_lower_copy(kv.second.get<std::string>("sha256", ""));
                pkg_out.files.push_back(std::move(f));
            }
        }
        return true;
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the manifest: %1%") % e.what()).str();
        return false;
    }
}

int unlayered_fork_number(const std::string &firmware_version)
{
    static const std::string tag = "+unlayered.";
    const size_t at = firmware_version.find(tag);
    if (at == std::string::npos)
        return -1;
    size_t i = at + tag.size();
    if (i >= firmware_version.size() || !std::isdigit(static_cast<unsigned char>(firmware_version[i])))
        return -1;
    long n = 0;
    for (; i < firmware_version.size() && std::isdigit(static_cast<unsigned char>(firmware_version[i])); ++i)
        n = std::min<long>(n * 10 + (firmware_version[i] - '0'), 1000000);
    return int(n);
}

// True when a G-code line, comment already removed, is an M558 selecting probe type 12. Parameters
// are whitespace separated in every file we ship; P{expr} is not recognised, and does not occur.
static bool is_m558_p12(const std::string &code)
{
    std::vector<std::string> words;
    boost::split(words, code, boost::is_any_of(" \t"), boost::token_compress_on);
    if (words.empty() || !boost::iequals(words.front(), "M558"))
        return false;
    for (size_t i = 1; i < words.size(); ++i)
        if (boost::iequals(words[i], "P12"))
            return true;
    return false;
}

std::string sys_required_firmware(const SysFilePackage &pkg, std::string *reason_out)
{
    std::string required = pkg.min_firmware;
    std::string reason   = required.empty() ? std::string() : "the package declares it";

    // What the files say and use, whatever the manifest says, keeping the highest:
    //  - sys/config.g's `var fwNeed = N`, the number its own first-boot firmware install tests;
    //  - M558 P12 anywhere, the differential Z probe, from +unlayered.12.
    // tools/make_sys_bundle.py applies the same two rules; this is where the next firmware-only
    // command goes when the configuration starts relying on one.
    static const int P12_FORK = 12;
    int              best     = unlayered_fork_number(required);
    for (const auto &f : pkg.files) {
        // Every file here is G-code; skip anything binary rather than scan it.
        if (std::find(f.data.begin(), f.data.end(), uint8_t(0)) != f.data.end())
            continue;
        const bool         is_config = boost::iequals(f.path, "sys/config.g");
        std::istringstream in(std::string(f.data.begin(), f.data.end()));
        std::string        line;
        while (std::getline(in, line)) {
            const size_t semi = line.find(';');
            if (semi != std::string::npos)
                line.erase(semi);
            boost::algorithm::trim(line);
            if (is_config && boost::istarts_with(line, "var fwNeed")) {
                const size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    const std::string num = boost::algorithm::trim_copy(line.substr(eq + 1));
                    if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                        const int n = std::min(std::atoi(num.c_str()), 1000000);
                        if (n > best) {
                            best     = n;
                            required = "+unlayered." + std::to_string(n);
                            reason   = "sys/config.g declares fwNeed";
                        }
                    }
                }
            } else if (best < P12_FORK && is_m558_p12(line)) {
                best     = P12_FORK;
                required = "+unlayered." + std::to_string(P12_FORK);
                reason   = f.path + " uses M558 P12, the differential Z probe";
            }
        }
    }

    if (reason_out)
        *reason_out = reason;
    return required;
}

bool sys_firmware_satisfies(const std::string &running, const std::string &required)
{
    if (required.empty())
        return true;
    const int need = unlayered_fork_number(required);
    if (need >= 0)
        // Only an Unlayered build has the fork's commands, so a stock board is always too old.
        return unlayered_fork_number(running) >= need;
    // A plain RRF version: semver order, ignoring build metadata, is correct here.
    const auto r = Semver::parse(running);
    const auto q = Semver::parse(required);
    if (!r || !q)
        return true; // cannot compare; not a reason to refuse
    return *r >= *q;
}

bool validate_sys_package(const SysFilePackage &pkg, std::string &error_out)
{
    if (pkg.files.empty()) {
        error_out = "The package contains no files to install";
        return false;
    }

    uint64_t total = 0;
    for (const auto &f : pkg.files) {
        std::string reason;
        std::string mapped;
        if (!sys_map_path(f.path, mapped, reason) || !boost::iequals(mapped, f.path)) {
            error_out = (boost::format("%1% is not a path this can write") % f.path).str();
            return false;
        }
        if (f.data.empty()) {
            error_out = (boost::format("%1% was not read") % f.path).str();
            return false;
        }
        if (f.data.size() > MAX_FILE_BYTES) {
            error_out = (boost::format("%1% is %2% bytes, which is far larger than a configuration "
                                       "file; this does not look like a configuration package")
                         % f.path % f.data.size()).str();
            return false;
        }
        if (!f.sha256.empty()) {
            const std::string actual = sha256_hex(f.data);
            if (!boost::iequals(actual, f.sha256)) {
                error_out = (boost::format("Checksum mismatch for %1%: expected %2%, got %3%")
                             % f.path % f.sha256 % actual).str();
                return false;
            }
        }
        total += f.data.size();
    }

    if (total > MAX_PACKAGE_BYTES) {
        error_out = (boost::format("The package is %1% bytes, which is too large to be printer "
                                   "configuration") % total).str();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// Marker
// -----------------------------------------------------------------------------------------------

static std::string local_timestamp()
{
    char       buffer[32];
    const auto t  = std::time(nullptr);
    const auto tm = *std::localtime(&t);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buffer);
}

std::string make_sys_marker_json(const SysFilePackage &pkg, const std::vector<std::string> &installed_paths)
{
    pt::ptree root;
    root.put("version", pkg.version);
    root.put("date", pkg.date);
    root.put("machine", pkg.machine);
    root.put("origin", pkg.origin);
    root.put("installedAt", local_timestamp());
    root.put("fileCount", installed_paths.size());

    pt::ptree files;
    for (const auto &p : installed_paths) {
        pt::ptree entry;
        entry.put("", p);
        files.push_back(std::make_pair("", entry));
    }
    root.add_child("files", files);

    std::ostringstream oss;
    pt::write_json(oss, root, /* pretty */ true);
    return oss.str();
}

bool parse_sys_marker(const std::string &json, SysFilesInstalled &out)
{
    try {
        pt::ptree root;
        std::istringstream iss(json);
        pt::read_json(iss, root);

        out.version      = root.get<std::string>("version", "");
        out.date         = root.get<std::string>("date", "");
        out.machine      = root.get<std::string>("machine", "");
        out.origin       = root.get<std::string>("origin", "");
        out.installed_at = root.get<std::string>("installedAt", "");
        out.file_count   = root.get<int>("fileCount", 0);
        out.known        = !out.version.empty() || !out.installed_at.empty();
        return out.known;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "SysFiles: could not parse the marker on the card: " << e.what();
        return false;
    }
}

// -----------------------------------------------------------------------------------------------
// Assembling a package out of raw (path, bytes) pairs
// -----------------------------------------------------------------------------------------------

namespace {

struct RawEntry
{
    std::string          path; // relative to the package root
    std::vector<uint8_t> data;
};

// Removes a directory that wraps the whole package - the "repo-1.2/" a GitHub archive adds, or the
// folder somebody zipped by right-clicking it. A wrapper that is itself a destination (sys/) is
// left alone, because that one carries meaning.
void strip_wrapper_dirs(std::vector<RawEntry> &entries)
{
    for (int pass = 0; pass < 3; ++pass) {
        std::string common;
        for (const auto &e : entries) {
            const auto slash = e.path.find('/');
            if (slash == std::string::npos)
                return; // something sits at the root, so there is no wrapper
            const std::string first = e.path.substr(0, slash);
            if (common.empty())
                common = first;
            else if (!boost::iequals(common, first))
                return;
        }
        if (common.empty() || is_allowed_root(common))
            return;

        BOOST_LOG_TRIVIAL(info) << "SysFiles: stripping wrapper directory " << common << "/";
        for (auto &e : entries)
            e.path = e.path.substr(common.size() + 1);
    }
}

bool build_package(std::vector<RawEntry> entries,
                   const std::string    &origin,
                   SysFilePackage       &pkg_out,
                   std::string          &error_out)
{
    for (auto &e : entries)
        e.path = normalize_separators(e.path);
    strip_wrapper_dirs(entries);

    SysFilePackage pkg;
    pkg.origin = origin;

    // A manifest at the package root, if there is one, is authoritative about what gets installed.
    bool have_manifest = false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!boost::iequals(it->path, "manifest.json") && !boost::iequals(it->path, "sys-manifest.json"))
            continue;
        const std::string json(it->data.begin(), it->data.end());
        if (!parse_sys_manifest(json, pkg, error_out))
            return false;
        pkg.origin   = origin;
        have_manifest = true;
        entries.erase(it);
        break;
    }

    // Everything the package actually carries, keyed by where it would land on the card.
    std::vector<SysFile> found;
    for (auto &e : entries) {
        if (sys_is_ignored_file(e.path)) {
            pkg.skipped.emplace_back(e.path, "not a configuration file");
            continue;
        }
        std::string card_path, reason;
        if (!sys_map_path(e.path, card_path, reason)) {
            pkg.skipped.emplace_back(e.path, reason);
            continue;
        }
        if (e.data.empty()) {
            // rr_upload has no way to create a zero length file, and an empty config macro is
            // almost always an accident rather than a deliberate no-op.
            pkg.skipped.emplace_back(e.path, "the file is empty");
            continue;
        }
        SysFile f;
        f.path   = card_path;
        f.origin = e.path;
        f.data   = std::move(e.data);
        found.push_back(std::move(f));
    }

    if (have_manifest && !pkg.files.empty()) {
        // Take the declared list, in the declared order, and fill it from what was found.
        std::vector<SysFile> resolved;
        for (auto &declared : pkg.files) {
            auto it = std::find_if(found.begin(), found.end(),
                                   [&declared](const SysFile &f) { return boost::iequals(f.path, declared.path); });
            if (it == found.end()) {
                error_out = (boost::format("The manifest lists %1%, but the package does not contain it")
                             % declared.path).str();
                return false;
            }
            it->sha256 = declared.sha256;
            resolved.push_back(std::move(*it));
            found.erase(it);
        }
        for (auto &leftover : found)
            pkg.skipped.emplace_back(leftover.origin, "not listed in manifest.json");
        pkg.files = std::move(resolved);
    } else {
        // No manifest, or one that does not enumerate files: install everything that was found.
        std::sort(found.begin(), found.end(),
                  [](const SysFile &a, const SysFile &b) { return a.path < b.path; });
        pkg.files = std::move(found);
    }

    if (pkg.files.empty()) {
        error_out = (boost::format("%1% contains no configuration files") % origin).str();
        return false;
    }

    pkg_out = std::move(pkg);
    return true;
}

bool read_zip_entries(mz_zip_archive       &arch,
                      std::vector<RawEntry> &out,
                      SysProgressFn          progress,
                      std::string           &error_out)
{
    const mz_uint count = mz_zip_reader_get_num_files(&arch);
    uint64_t      total = 0;

    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&arch, i))
            continue;

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&arch, i, &stat)) {
            error_out = "The archive could not be read";
            return false;
        }
        if (stat.m_uncomp_size > MAX_FILE_BYTES) {
            error_out = (boost::format("%1% in the archive is %2% bytes, which is far too large to be "
                                       "a configuration file") % stat.m_filename % uint64_t(stat.m_uncomp_size)).str();
            return false;
        }
        total += stat.m_uncomp_size;
        if (total > MAX_PACKAGE_BYTES) {
            error_out = "The archive is too large to be printer configuration";
            return false;
        }

        RawEntry entry;
        entry.path = stat.m_filename;
        entry.data.resize(size_t(stat.m_uncomp_size));
        if (stat.m_uncomp_size > 0 &&
            !mz_zip_reader_extract_to_mem(&arch, i, entry.data.data(), entry.data.size(), 0)) {
            error_out = (boost::format("Could not extract %1% from the archive") % stat.m_filename).str();
            return false;
        }

        out.push_back(std::move(entry));
        if (progress && !progress("Reading the archive", i + 1, count)) {
            error_out = "Cancelled";
            return false;
        }
    }

    if (out.empty()) {
        error_out = "The archive is empty";
        return false;
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------------------------
// LocalSysFileSource
// -----------------------------------------------------------------------------------------------

LocalSysFileSource::LocalSysFileSource(std::string path) : m_path(std::move(path)) {}

std::string LocalSysFileSource::description() const
{
    return (boost::format("Local files (%1%)") % m_path).str();
}

bool LocalSysFileSource::fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    boost::system::error_code ec;
    const fs::path            p = fs::path(m_path);

    if (!fs::exists(p, ec)) {
        error_out = (boost::format("%1% does not exist") % m_path).str();
        return false;
    }
    const bool ok = fs::is_directory(p, ec) ? fetch_from_directory(pkg_out, progress, error_out)
                                            : fetch_from_zip(pkg_out, progress, error_out);
    if (!ok)
        return false;

    // Nothing about a hand-picked folder states a version, so the caller treats it the way the
    // firmware updater treats a hand-picked image: always offered, never called up to date.
    if (pkg_out.notes.empty())
        pkg_out.notes = (boost::format("Configuration files from %1%.") % m_path).str();
    return true;
}

bool LocalSysFileSource::fetch_from_zip(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    MZ_Archive zip;
    if (!open_zip_reader(&zip.arch, m_path)) {
        error_out = (boost::format("%1% could not be opened as a zip archive") % m_path).str();
        return false;
    }

    std::vector<RawEntry> entries;
    const bool            read_ok = read_zip_entries(zip.arch, entries, progress, error_out);
    close_zip_reader(&zip.arch);
    if (!read_ok)
        return false;

    return build_package(std::move(entries), description(), pkg_out, error_out);
}

bool LocalSysFileSource::fetch_from_directory(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    const fs::path root = fs::path(m_path);
    // Picking the machine's sys folder itself has to mean "these are the sys files", so the folder
    // name is kept as the root when it names a destination.
    const std::string root_name   = root.filename().string();
    const std::string path_prefix = is_allowed_root(root_name) ? canonical_root(root_name) + "/" : std::string();

    std::vector<RawEntry> entries;
    uint64_t              total = 0;

    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error_out = (boost::format("Could not read %1%: %2%") % m_path % ec.message()).str();
            return false;
        }
        if (fs::is_directory(it->path(), ec))
            continue;

        // Relative to the folder the user picked.
        std::string rel = it->path().string().substr(root.string().size());
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
            rel = rel.substr(1);

        RawEntry entry;
        entry.path = path_prefix + normalize_separators(rel);

        const uint64_t size = uint64_t(fs::file_size(it->path(), ec));
        if (ec) {
            error_out = (boost::format("Could not read %1%: %2%") % it->path().string() % ec.message()).str();
            return false;
        }
        if (size > MAX_FILE_BYTES) {
            // Not fatal for a folder: the user may simply have a stray archive sitting in it.
            BOOST_LOG_TRIVIAL(info) << "SysFiles: ignoring oversized file " << it->path().string();
            continue;
        }
        total += size;
        if (total > MAX_PACKAGE_BYTES) {
            error_out = (boost::format("%1% holds more data than printer configuration ever should; "
                                       "check that it is the right folder") % m_path).str();
            return false;
        }

        std::string read_err;
        if (!DuetRrfClient::read_file_binary(it->path().string(), entry.data, read_err)) {
            error_out = read_err;
            return false;
        }
        entries.push_back(std::move(entry));

        if (progress && !progress("Reading the folder", entries.size(), entries.size())) {
            error_out = "Cancelled";
            return false;
        }
    }

    if (entries.empty()) {
        error_out = (boost::format("%1% is empty") % m_path).str();
        return false;
    }
    return build_package(std::move(entries), description(), pkg_out, error_out);
}

// -----------------------------------------------------------------------------------------------
// GitHubSysFileSource
// -----------------------------------------------------------------------------------------------

GitHubSysFileSource::GitHubSysFileSource(Options opts) : m_opts(std::move(opts)) {}

std::string GitHubSysFileSource::description() const
{
    return (boost::format("GitHub release %1%/%2% (%3%)")
            % m_opts.owner % m_opts.repo % (m_opts.tag.empty() ? std::string("latest") : m_opts.tag)).str();
}

bool GitHubSysFileSource::fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    // /releases/latest skips pre-releases, so list them and take the newest published one instead.
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

    auto github_headers = [this](Http &http) {
        http.header("User-Agent", "UnlayeredSlicer");
        if (!m_opts.token.empty())
            http.header("Authorization", std::string("Bearer ") + m_opts.token);
    };

    std::string body;
    bool        ok   = false;
    auto        http = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(30)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28");
    github_headers(http);
    http.on_error([&](std::string b, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("SysFiles: GitHub request failed: %1%, HTTP %2%") % error % status;
            if (status == 404)
                error_out = (boost::format("No configuration releases found at %1%/%2%") % m_opts.owner % m_opts.repo).str();
            // The gated feed distinguishes the two cases and so must this: 401 means no token was
            // sent at all, 403 means one was sent and refused. Both are failures a customer can
            // act on, and neither reads that way as a bare HTTP code -- "HTTP 401" looks like a
            // fault in the slicer rather than a setting that was never filled in, which is the
            // normal first-run state.
            else if (status == 401)
                error_out = "Configuration updates need an access token. Open your account page on "
                            "the Unlayered website, copy the slicer access token, and paste it into "
                            "Preferences > Printer updates.";
            else if (status == 403 && !m_opts.token.empty())
                error_out = "That access token was not accepted. Check it against the one on your "
                            "account page -- it may have been replaced, or the account it belongs "
                            "to may no longer have access to configuration updates.";
            else if (status == 403)
                error_out = "GitHub rejected the request (rate limited, or the repository is private)";
            else
                error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string b, unsigned) { body = std::move(b); ok = true; })
        .perform_sync();

    if (!ok)
        return false;

    std::string tag;
    std::string release_notes;
    std::string asset_url;
    std::string asset_name;
    try {
        pt::ptree root;
        std::istringstream iss(body);
        pt::read_json(iss, root);

        // A tag or "latest" query returns one object; a list query returns an array.
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
                error_out = (boost::format("No published configuration releases at %1%/%2%")
                             % m_opts.owner % m_opts.repo).str();
                return false;
            }
        } else {
            release = &root;
        }

        tag           = release->get<std::string>("tag_name", "");
        release_notes = release->get<std::string>("body", "");

        // Prefer the asset the options name; fall back to any zip, so a release that calls it
        // something else still works.
        if (auto assets = release->get_child_optional("assets")) {
            for (const auto &kv : *assets) {
                const std::string name = kv.second.get<std::string>("name", "");
                const std::string durl = kv.second.get<std::string>("browser_download_url", "");
                if (name.empty() || durl.empty())
                    continue;
                if (boost::iequals(name, m_opts.asset_name)) {
                    asset_name = name;
                    asset_url  = durl;
                    break;
                }
                if (asset_url.empty() && boost::iends_with(name, ".zip")) {
                    asset_name = name;
                    asset_url  = durl;
                }
            }
        }
        if (asset_url.empty()) {
            error_out = (boost::format("Release %1% has no configuration archive attached") % tag).str();
            return false;
        }
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the GitHub release response: %1%") % e.what()).str();
        return false;
    }

    std::vector<uint8_t> zip_bytes;
    bool                 download_ok = false;
    bool                 cancelled   = false;
    auto                 dl          = Http::get(asset_url);
    dl.timeout_connect(10)
      .timeout_max(120)
      .header("Accept", "application/octet-stream");
    github_headers(dl);
    dl.on_error([&](std::string b, std::string error, unsigned status) {
            if (!cancelled)
                error_out = (boost::format("Could not download %1%: %2%") % asset_name
                             % (error.empty() ? (boost::format("HTTP %1%") % status).str() : error)).str();
        })
        .on_complete([&](std::string b, unsigned) {
            zip_bytes.assign(b.begin(), b.end());
            download_ok = true;
        })
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && !progress((boost::format("Downloading %1%") % asset_name).str(), p.dlnow, p.dltotal)) {
                cancel      = true;
                cancelled   = true;
                error_out   = "Cancelled";
            }
        })
        .perform_sync();

    if (!download_ok)
        return false;
    if (zip_bytes.empty()) {
        error_out = (boost::format("%1% is empty") % asset_name).str();
        return false;
    }

    MZ_Archive zip;
    if (!mz_zip_reader_init_mem(&zip.arch, zip_bytes.data(), zip_bytes.size(), 0)) {
        error_out = (boost::format("%1% is not a readable zip archive") % asset_name).str();
        return false;
    }
    std::vector<RawEntry> entries;
    const bool            read_ok = read_zip_entries(zip.arch, entries, progress, error_out);
    mz_zip_reader_end(&zip.arch);
    if (!read_ok)
        return false;

    const std::string origin = (boost::format("GitHub release %1% (%2%/%3%)") % tag % m_opts.owner % m_opts.repo).str();
    if (!build_package(std::move(entries), origin, pkg_out, error_out))
        return false;

    // A manifest inside the archive is authoritative; without one the tag stands in, exactly as it
    // does for a firmware release with no manifest.
    if (pkg_out.version.empty())
        pkg_out.version = boost::starts_with(tag, "v") ? tag.substr(1) : tag;
    if (pkg_out.notes.empty())
        pkg_out.notes = release_notes;
    return true;
}

} // namespace Slic3r
