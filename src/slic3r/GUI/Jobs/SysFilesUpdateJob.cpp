#include "SysFilesUpdateJob.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <set>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SYS_FILES_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SYS_FILES_UPDATE_FINISHED, wxCommandEvent);

// A Duet reboots in well under a minute. Beyond this something is wrong and the user needs to be
// told rather than left watching a spinner.
static const int RESTART_TIMEOUT_SECONDS = 120;

// Machine states in which rewriting the configuration would interfere with a running job. RRF does
// not stop us: macros are read from the card while a print runs, so replacing one mid-print can
// change behaviour halfway through.
static bool is_busy_status(const std::string &status)
{
    static const std::set<std::string> busy = {"processing", "pausing", "paused", "resuming", "cancelling", "simulating"};
    return busy.count(boost::algorithm::to_lower_copy(status)) > 0;
}

// Host names go into a directory name, so anything a file system might object to is flattened.
static std::string sanitize_for_path(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_');
    while (!out.empty() && out.front() == '_')
        out.erase(out.begin());
    return out.empty() ? std::string("printer") : out;
}

static std::string backup_timestamp()
{
    char       buffer[32];
    const auto t  = std::time(nullptr);
    const auto tm = *std::localtime(&t);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H%M%S", &tm);
    return std::string(buffer);
}

SysFilesUpdateJob::SysFilesUpdateJob(DuetClientFactory client_factory, SourceFactory source_factory, Options options)
    : m_client_factory(std::move(client_factory))
    , m_source_factory(std::move(source_factory))
    , m_options(std::move(options))
{}

void SysFilesUpdateJob::update_status(Ctl &ctl, int percent, const std::string &msg)
{
    BOOST_LOG_TRIVIAL(info) << "SysFilesUpdateJob: " << percent << "% " << msg;
    ctl.update_status(percent, msg);

    if (!m_event_handle || !ui_alive())
        return;

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    if (percent > 0 && percent < 100 && now_ms - m_last_post_ms < 100)
        return;
    m_last_post_ms = now_ms;

    wxCommandEvent event(EVT_SYS_FILES_UPDATE_MESSAGE);
    event.SetString(wxString::FromUTF8(msg.c_str()));
    event.SetInt(percent);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void SysFilesUpdateJob::fail(Ctl &ctl, const std::string &msg)
{
    BOOST_LOG_TRIVIAL(error) << "SysFilesUpdateJob failed: " << msg;
    m_result.outcome = Outcome::Failed;
    m_result.message = msg;
    update_status(ctl, 100, msg);
}

bool SysFilesUpdateJob::make_backup(Ctl &ctl, DuetClient &client, const SysFilePackage &pkg, std::string &error_out)
{
    // Name the folder after the machine rather than the connection. An IP address is a reasonable
    // identity but a COM port is not - it changes with the USB socket, and it changes again when a
    // firmware update alters how many ports the board presents. network.name (what M550 set) is the
    // only field that tells two Unlayered machines apart, so prefer it and fall back to the endpoint.
    const std::string key = (m_result.board_known && !m_result.board.machine_name.empty())
                                ? m_result.board.machine_name
                                : client.endpoint();
    const fs::path dir = fs::path(data_dir()) / "printer_backups" / sanitize_for_path(key) / backup_timestamp();

    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error_out = (boost::format("Could not create the backup folder %1%: %2%") % dir.string() % ec.message()).str();
        return false;
    }

    int      saved = 0;
    unsigned index = 0;
    for (const auto &f : pkg.files) {
        if (ctl.was_canceled()) {
            error_out = "Cancelled";
            return false;
        }
        ++index;
        update_status(ctl, int(35 + 15.0 * double(index) / double(std::max<size_t>(pkg.files.size(), 1))),
                      (boost::format(_u8L("Backing up %1%")) % f.path).str());

        std::vector<uint8_t> current;
        bool                 missing = false;
        std::string          err;
        if (!client.download(f.remote_path(), current, missing, err)) {
            if (missing)
                continue; // A file that is not there yet has nothing to back up.
            error_out = (boost::format("Could not read %1% from the printer: %2%") % f.path % err).str();
            return false;
        }
        if (missing)
            continue;

        const fs::path out = dir / fs::path(f.path);
        fs::create_directories(out.parent_path(), ec);
        std::ofstream os(out.string(), std::ios::binary);
        if (!os) {
            error_out = (boost::format("Could not write %1%") % out.string()).str();
            return false;
        }
        if (!current.empty())
            os.write(reinterpret_cast<const char *>(current.data()), current.size());
        os.close();
        ++saved;
    }

    m_result.backup_dir = dir.string();
    BOOST_LOG_TRIVIAL(info) << boost::format("SysFilesUpdateJob: backed up %1% file(s) to %2%") % saved % dir.string();

    // An install onto a fresh card replaces nothing, so an empty backup folder is not a failure.
    if (saved == 0) {
        fs::remove(dir, ec);
        m_result.backup_dir.clear();
    }
    return true;
}

void SysFilesUpdateJob::process(Ctl &ctl)
{
    m_result = Result();

    // Built here, on the worker thread: the client owns an OS handle (a socket or a serial port)
    // and is confined to the thread that created it.
    std::unique_ptr<DuetClient> client_owner = m_client_factory ? m_client_factory() : nullptr;
    if (!client_owner) {
        m_result.outcome = Outcome::Failed;
        m_result.message = _u8L("No connection to the printer was configured.");
        return;
    }
    DuetClient &client = *client_owner;
    std::string   err;

    // ---- 1. Connect -----------------------------------------------------------------------
    update_status(ctl, 2, _u8L("Connecting to the printer"));
    switch (client.connect(err)) {
    case DuetClient::ConnectResult::ok:
        break;
    case DuetClient::ConnectResult::wrong_password:
        fail(ctl, _u8L("The printer rejected the password. Check the printer's password in its settings."));
        return;
    case DuetClient::ConnectResult::no_free_session:
        fail(ctl, _u8L("The printer has no free connection slots. Close any open Duet Web Control tabs and try again."));
        return;
    case DuetClient::ConnectResult::unreachable:
    default:
        fail(ctl, (boost::format(_u8L("Could not reach the printer at %1%: %2%")) % client.endpoint() % err).str());
        return;
    }

    // ---- 2. Identify the printer ------------------------------------------------------------
    update_status(ctl, 5, _u8L("Reading printer information"));
    DuetClient::BoardInfo board;
    if (client.read_board_info(board, err)) {
        m_result.board       = board;
        m_result.board_known = true;
    } else {
        // Not fatal: the board description is only used to label the dialog, and the files can be
        // written without it.
        BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdateJob: could not read board info: " << err;
    }

    // ---- 3. What is on the card already? ------------------------------------------------------
    update_status(ctl, 7, _u8L("Checking what is installed on the printer"));
    {
        std::vector<uint8_t> marker;
        bool                 missing = false;
        std::string          marker_err;
        if (client.download(std::string("0:/") + SYS_MARKER_PATH, marker, missing, marker_err) && !missing) {
            const std::string json(marker.begin(), marker.end());
            parse_sys_marker(json, m_result.installed);
        } else if (!missing && !marker_err.empty()) {
            BOOST_LOG_TRIVIAL(info) << "SysFilesUpdateJob: no installed-version marker: " << marker_err;
        }
    }

    // ---- 4. Preflight: do not rewrite a printer's configuration mid-job -----------------------
    std::string status;
    if (!client.read_status(status, err)) {
        fail(ctl, (boost::format(_u8L("Could not read the printer's status: %1%")) % err).str());
        return;
    }
    if (!m_options.query_only && is_busy_status(status)) {
        fail(ctl, (boost::format(_u8L("The printer is busy (%1%). Stop or finish the current job before "
                                      "changing its configuration.")) % status).str());
        return;
    }
    if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

    // ---- 5. Fetch the package -----------------------------------------------------------------
    std::unique_ptr<SysFileSource> source = m_source_factory ? m_source_factory() : nullptr;
    if (!source) {
        fail(ctl, _u8L("No source of configuration files is configured."));
        return;
    }

    update_status(ctl, 8, (boost::format(_u8L("Looking for configuration files (%1%)")) % source->description()).str());

    auto fetch_progress = [&](const std::string &stage, uint64_t done, uint64_t total) -> bool {
        if (ctl.was_canceled())
            return false;
        const int pct = total > 0 ? int(8 + (27.0 * double(done) / double(total))) : 8;
        update_status(ctl, std::min(pct, 35), stage);
        return true;
    };

    SysFilePackage pkg;
    if (!source->fetch(pkg, fetch_progress, err)) {
        if (ctl.was_canceled() || err == "Cancelled") { m_result.outcome = Outcome::Cancelled; return; }
        fail(ctl, (boost::format(_u8L("Could not get the configuration files: %1%")) % err).str());
        return;
    }

    m_result.available_version = pkg.version;
    m_result.available_date    = pkg.date;
    m_result.available_machine = pkg.machine;
    m_result.available_notes   = pkg.notes;
    m_result.available_origin  = pkg.origin.empty() ? source->description() : pkg.origin;
    m_result.skipped           = pkg.skipped;
    for (const auto &f : pkg.files)
        m_result.available_files.push_back({f.path, uint64_t(f.data.size())});

    m_result.required_firmware = sys_required_firmware(pkg, &m_result.required_firmware_reason);
    m_result.firmware_too_old  = m_result.board_known &&
                                 !sys_firmware_satisfies(m_result.board.firmware_version, m_result.required_firmware);

    if (m_options.query_only) {
        m_result.outcome = Outcome::QueryOk;
        update_status(ctl, 100, _u8L("Ready"));
        client.disconnect();
        return;
    }

    // ---- 6. Narrow it down to what the user ticked ---------------------------------------------
    if (!m_options.selected.empty()) {
        std::vector<SysFile> kept;
        for (const auto &want : m_options.selected) {
            auto it = std::find_if(pkg.files.begin(), pkg.files.end(),
                                   [&want](const SysFile &f) { return boost::iequals(f.path, want); });
            if (it == pkg.files.end()) {
                // The package can legitimately have changed between the query and the install.
                BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdateJob: " << want << " is no longer in the package";
                m_result.skipped.emplace_back(want, "no longer in the package");
                continue;
            }
            kept.push_back(std::move(*it));
            pkg.files.erase(it);
        }
        pkg.files = std::move(kept);
    }

    if (pkg.files.empty()) {
        fail(ctl, _u8L("There are no files selected to install."));
        return;
    }

    // Configuration newer than the firmware is worse than no update: config.g stops at the first
    // command the board does not know and skips the rest, so the machine comes up with no heaters,
    // fans or tool (a customer hit exactly this with M558 P12 on +unlayered.9, 2026-09-27). Judged
    // on the ticked files, so leaving the offending file out is a way through. Not overridable by
    // force: the fix is always to update the firmware first, and that is always possible.
    {
        std::string       reason;
        const std::string need = sys_required_firmware(pkg, &reason);
        if (!m_result.board_known) {
            if (!need.empty())
                BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdateJob: these files need firmware " << need
                                           << " (" << reason << ") and the board's version could not be read";
        } else if (!sys_firmware_satisfies(m_result.board.firmware_version, need)) {
            fail(ctl, (boost::format(_u8L("These files need printer firmware %1% or newer (%2%), and the printer is "
                                          "running %3%. Nothing has been changed. Update the firmware first with "
                                          "the Firmware button, then install the configuration."))
                       % need % reason % m_result.board.firmware_version).str());
            return;
        }
    }

    // A package that states no version is never "already installed" - the same rule the firmware
    // updater applies to a hand-picked image.
    if (!m_options.force && !pkg.version.empty() && m_result.installed.known &&
        pkg.version == m_result.installed.version &&
        (pkg.date.empty() || m_result.installed.date.empty() || pkg.date == m_result.installed.date)) {
        m_result.outcome = Outcome::UpToDate;
        m_result.message = (boost::format(_u8L("The printer already has configuration %1%.")) % pkg.version).str();
        update_status(ctl, 100, m_result.message);
        client.disconnect();
        return;
    }

    update_status(ctl, 36, _u8L("Checking the configuration files"));
    if (!validate_sys_package(pkg, err)) {
        fail(ctl, err);
        return;
    }
    if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

    // ---- 7. Back up whatever is being replaced ---------------------------------------------------
    if (m_options.backup) {
        if (!make_backup(ctl, client, pkg, err)) {
            if (ctl.was_canceled() || err == "Cancelled") { m_result.outcome = Outcome::Cancelled; return; }
            fail(ctl, (boost::format(_u8L("Could not back up the printer's current files: %1%. "
                                          "Nothing has been changed.")) % err).str());
            return;
        }
    }
    if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

    // ---- 8. Make sure the destinations exist -----------------------------------------------------
    // rr_upload cannot create a missing directory, and 0:/filaments or a macro subfolder may well
    // not exist yet. rr_mkdir answers err=1 for "already there" too, so its result is only logged.
    {
        std::set<std::string> dirs;
        for (const auto &f : pkg.files) {
            const std::string remote = f.remote_path();
            const auto        slash  = remote.find_last_of('/');
            if (slash != std::string::npos && slash > 2)
                dirs.insert(remote.substr(0, slash));
        }
        // Parents before children, so a nested macro folder can be created a level at a time.
        for (const auto &d : dirs) {
            std::string mkdir_err;
            if (!client.mkdir(d, mkdir_err))
                BOOST_LOG_TRIVIAL(debug) << "SysFilesUpdateJob: mkdir " << d << ": " << mkdir_err
                                         << " (expected when it already exists)";
        }
    }

    // ---- 9. Upload -------------------------------------------------------------------------------
    const uint64_t total_bytes = std::max<uint64_t>(pkg.total_bytes(), 1);
    uint64_t       sent_bytes  = 0;

    for (const auto &f : pkg.files) {
        if (ctl.was_canceled()) {
            m_result.outcome = Outcome::Cancelled;
            m_result.message = m_result.written.empty()
                ? _u8L("Cancelled. Nothing was changed on the printer.")
                : (boost::format(_u8L("Cancelled after writing %1% file(s). The printer's configuration is "
                                      "part way through an update.")) % m_result.written.size()).str();
            return;
        }

        const uint64_t base = sent_bytes;
        update_status(ctl, int(50 + 40.0 * double(base) / double(total_bytes)),
                      (boost::format(_u8L("Writing %1%")) % f.path).str());

        auto on_progress = [&](uint64_t sent, uint64_t, bool &cancel) {
            if (ctl.was_canceled()) { cancel = true; return; }
            const int pct = int(50 + 40.0 * double(base + sent) / double(total_bytes));
            update_status(ctl, std::min(pct, 90), (boost::format(_u8L("Writing %1%")) % f.path).str());
        };

        if (!client.upload(f.remote_path(), f.data, on_progress, err)) {
            if (ctl.was_canceled() || err == "Cancelled") { m_result.outcome = Outcome::Cancelled; return; }
            std::string msg = (boost::format(_u8L("Could not write %1%: %2%.")) % f.path % err).str();
            if (!m_result.written.empty())
                msg += " " + (boost::format(_u8L("%1% file(s) had already been written; the printer's "
                                                 "configuration is part way through an update."))
                              % m_result.written.size()).str();
            if (!m_result.backup_dir.empty())
                msg += " " + (boost::format(_u8L("The previous files are in %1%.")) % m_result.backup_dir).str();
            fail(ctl, msg);
            return;
        }
        m_result.written.push_back(f.path);
        sent_bytes += f.data.size();
    }

    // ---- 10. Record what was installed -------------------------------------------------------------
    // Best effort: the files are already on the card, and a missing marker only costs the next
    // query its "installed version" line.
    update_status(ctl, 92, _u8L("Recording the installed version"));
    {
        const std::string   json = make_sys_marker_json(pkg, m_result.written);
        std::vector<uint8_t> data(json.begin(), json.end());
        std::string          marker_err;
        if (!client.upload(std::string("0:/") + SYS_MARKER_PATH, data, nullptr, marker_err))
            BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdateJob: could not write the version marker: " << marker_err;
    }

    m_result.outcome = Outcome::Success;

    // ---- 11. Restart ---------------------------------------------------------------------------------
    if (!m_options.restart) {
        m_result.message = (boost::format(_u8L("Wrote %1% file(s). Restart the printer for the new "
                                               "configuration to take effect."))
                            % m_result.written.size()).str();
        update_status(ctl, 100, m_result.message);
        client.disconnect();
        return;
    }

    update_status(ctl, 93, _u8L("Restarting the printer"));
    if (!client.gcode("M999", err)) {
        // The files are installed; only the restart failed, so this is a warning rather than a
        // failed update.
        m_result.message = (boost::format(_u8L("Wrote %1% file(s), but the printer did not accept the "
                                               "restart command: %2%. Restart it yourself for the new "
                                               "configuration to take effect."))
                            % m_result.written.size() % err).str();
        update_status(ctl, 100, m_result.message);
        client.disconnect();
        return;
    }

    auto should_cancel = [&ctl]() { return ctl.was_canceled(); };
    auto on_tick       = [&](int elapsed) {
        update_status(ctl, 93 + std::min(elapsed / 20, 5),
                      (boost::format(_u8L("Waiting for the printer to restart (%1%s)")) % elapsed).str());
    };

    if (!client.wait_for_reboot(RESTART_TIMEOUT_SECONDS, should_cancel, on_tick, err)) {
        m_result.message = ctl.was_canceled()
            ? (boost::format(_u8L("Wrote %1% file(s). Stopped waiting for the restart; the printer is "
                                  "restarting on its own.")) % m_result.written.size()).str()
            : (boost::format(_u8L("Wrote %1% file(s), but the printer did not come back after %2% seconds. "
                                  "Check it: a configuration file it cannot parse will stop it starting up."))
               % m_result.written.size() % RESTART_TIMEOUT_SECONDS).str();
        // The files did get written, so this is not a failed install - but the user has to look at
        // the machine, so do not call it a success either.
        m_result.outcome = ctl.was_canceled() ? Outcome::Cancelled : Outcome::Failed;
        update_status(ctl, 100, m_result.message);
        return;
    }

    m_result.restarted = true;
    m_result.message   = (boost::format(_u8L("Wrote %1% file(s) and restarted the printer."))
                          % m_result.written.size()).str();
    if (!m_result.backup_dir.empty())
        m_result.message += " " + (boost::format(_u8L("The previous files were saved to %1%."))
                                   % m_result.backup_dir).str();
    update_status(ctl, 100, m_result.message);
    client.disconnect();
}

void SysFilesUpdateJob::finalize(bool canceled, std::exception_ptr &e)
{
    if (canceled && m_result.outcome != Outcome::Failed)
        m_result.outcome = Outcome::Cancelled;

    if (e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            m_result.outcome = Outcome::Failed;
            m_result.message = (boost::format(_u8L("Updating the printer's files failed unexpectedly: %1%")) % ex.what()).str();
            BOOST_LOG_TRIVIAL(error) << "SysFilesUpdateJob: unhandled exception: " << ex.what();
        }
        // Handled here; do not let the worker rethrow and stack a second, generic error dialog.
        e = nullptr;
    }

    if (m_result_slot)
        *m_result_slot = m_result;

    if (m_event_handle && ui_alive()) {
        wxCommandEvent event(EVT_SYS_FILES_UPDATE_FINISHED);
        event.SetInt(int(m_result.outcome));
        event.SetString(wxString::FromUTF8(m_result.message.c_str()));
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
    }
}

}} // namespace Slic3r::GUI
