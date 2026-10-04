#include "FirmwareUpdateJob.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_FIRMWARE_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_FIRMWARE_UPDATE_FINISHED, wxCommandEvent);

// How long to wait for the board to reappear after M997. DuetWebControl polls indefinitely; a
// Mini 5+ normally reboots well inside a minute, so anything past this is a real failure the user
// needs to hear about rather than a spinner that never stops.
static const int REBOOT_TIMEOUT_SECONDS = 180;

// Machine states in which flashing would abandon a running job. RepRapFirmware itself will not
// refuse M997 here, so this is the only thing standing between a user and a ruined print.
static bool is_busy_status(const std::string &status)
{
    static const std::set<std::string> busy = {"processing", "pausing", "paused", "resuming", "cancelling", "simulating"};
    return busy.count(boost::algorithm::to_lower_copy(status)) > 0;
}

FirmwareUpdateJob::FirmwareUpdateJob(DuetClientFactory client_factory,
                                     SourceFactory source_factory,
                                     Selection     selection,
                                     bool          force,
                                     bool          query_only)
    : m_client_factory(std::move(client_factory))
    , m_source_factory(std::move(source_factory))
    , m_selection(std::move(selection))
    , m_force(force)
    , m_query_only(query_only)
{}

void FirmwareUpdateJob::update_status(Ctl &ctl, int percent, const std::string &msg)
{
    BOOST_LOG_TRIVIAL(info) << "FirmwareUpdateJob: " << percent << "% " << msg;
    ctl.update_status(percent, msg);

    if (!m_event_handle || !ui_alive())
        return;

    // Coalesce rapid updates. Milestones (0 and 100) always go out so the UI never misses the
    // start or the end of a step.
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    if (percent > 0 && percent < 100 && now_ms - m_last_post_ms < 100)
        return;
    m_last_post_ms = now_ms;

    wxCommandEvent event(EVT_FIRMWARE_UPDATE_MESSAGE);
    event.SetString(wxString::FromUTF8(msg.c_str()));
    event.SetInt(percent);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void FirmwareUpdateJob::fail(Ctl &ctl, const std::string &msg, bool recovery_hint)
{
    BOOST_LOG_TRIVIAL(error) << "FirmwareUpdateJob failed: " << msg;
    m_result.outcome             = Outcome::Failed;
    m_result.message             = msg;
    m_result.needs_recovery_hint = recovery_hint;
    update_status(ctl, 100, msg);
}

void FirmwareUpdateJob::process(Ctl &ctl)
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

    // ---- 2. Identify the board ------------------------------------------------------------
    update_status(ctl, 5, _u8L("Reading printer information"));
    DuetClient::BoardInfo board;
    if (!client.read_board_info(board, err)) {
        fail(ctl, (boost::format(_u8L("Could not read the printer's firmware information: %1%")) % err).str());
        return;
    }
    m_result.version_before = board.firmware_version;
    m_result.board          = board;
    m_result.board_known    = true;
    BOOST_LOG_TRIVIAL(info) << boost::format("FirmwareUpdateJob: board %1% (%2%) running %3% built %4%")
                                   % board.name % board.short_name % board.firmware_version % board.firmware_date;

    // ---- 3. Preflight: refuse to interrupt a job -------------------------------------------
    std::string status;
    if (!client.read_status(status, err)) {
        fail(ctl, (boost::format(_u8L("Could not read the printer's status: %1%")) % err).str());
        return;
    }
    // A query is read-only, so it is allowed while the printer is busy; only the flash is not.
    if (!m_query_only && is_busy_status(status)) {
        fail(ctl, (boost::format(_u8L("The printer is busy (%1%). Stop or finish the current job before updating firmware."))
                   % status).str());
        return;
    }
    if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

    // The source can only be built now: matching release assets, and naming the files we upload,
    // both depend on what this particular board calls its own firmware.
    std::unique_ptr<FirmwareSource> source = m_source_factory ? m_source_factory(board) : nullptr;
    if (!source) {
        fail(ctl, _u8L("No firmware source is configured."));
        return;
    }

    // ---- 4/5. Fetch the firmware and check it ----------------------------------------------
    update_status(ctl, 8, (boost::format(_u8L("Looking for firmware (%1%)")) % source->description()).str());

    FirmwarePackage pkg;

    if (m_query_only) {
        // Metadata only: enough to tell the user what is available, with no payload transfer.
        if (!source->fetch_metadata(pkg, err)) {
            if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }
            fail(ctl, (boost::format(_u8L("Could not check for firmware: %1%")) % err).str());
            return;
        }
        m_result.available_version = pkg.version;
        m_result.available_notes   = pkg.notes;
        m_result.available_origin  = pkg.origin.empty() ? source->description() : pkg.origin;
        for (const auto &img : pkg.images)
            m_result.available_roles.push_back(img.role);
        // Ask the machine which display it has, so the dialog can offer the right image rather
        // than both. Read-only, and a failure here only means the display row has to ask.
        m_result.display = detect_display(client);
        BOOST_LOG_TRIVIAL(info) << "FirmwareUpdateJob: display detection: " << m_result.display.evidence;
        m_result.outcome           = Outcome::QueryOk;
        update_status(ctl, 100, _u8L("Ready"));
        client.disconnect();
        return;
    }

    auto fetch_progress = [&](const std::string &stage, uint64_t done, uint64_t total) -> bool {
        if (ctl.was_canceled())
            return false;
        // Fetching occupies 8..40% of the bar.
        const int pct = total > 0 ? int(8 + (32.0 * double(done) / double(total))) : 8;
        update_status(ctl, std::min(pct, 40), stage);
        return true;
    };
    if (!source->fetch(pkg, fetch_progress, err)) {
        if (ctl.was_canceled() || err == "Cancelled") { m_result.outcome = Outcome::Cancelled; return; }
        fail(ctl, (boost::format(_u8L("Could not get the firmware: %1%")) % err).str());
        return;
    }
    m_result.available_version = pkg.version;
    m_result.available_origin  = pkg.origin.empty() ? source->description() : pkg.origin;

    // A source that cannot state a version (a hand-picked file) is always offered, never skipped.
    // Version strings are compared exactly: the Unlayered fork identifies itself in semver build
    // metadata, which semver comparison ignores entirely.
    //
    // This describes the MAINBOARD and nothing else. The board reports its own firmware version
    // and no other module's, and RRF stores no version for either display - a PanelDue announces
    // its build in a boot beacon and the panel does so on connect, neither of which is readable
    // from here. So a display can never be shown to be current, and is flashed whenever asked for.
    const bool board_current = !pkg.version.empty() && pkg.version == board.firmware_version &&
                               (pkg.build_date.empty() || board.firmware_date.empty() ||
                                pkg.build_date == board.firmware_date);

    Selection selection = m_selection;
    if (!m_force && board_current) {
        if (selection.display == FirmwareRole::Unknown) {
            m_result.outcome       = Outcome::UpToDate;
            m_result.version_after = board.firmware_version;
            m_result.message       = (boost::format(_u8L("The printer is already running %1%.")) % board.firmware_version).str();
            update_status(ctl, 100, m_result.message);
            client.disconnect();
            return;
        }
        // The board is current but a display was asked for. Drop the mainboard instead of putting
        // the printer through a reboot to install the firmware it is already running.
        if (selection.mainboard) {
            BOOST_LOG_TRIVIAL(info) << "FirmwareUpdateJob: board already on " << board.firmware_version
                                    << "; updating the display only";
            selection.mainboard = false;
        }
    }

    update_status(ctl, 42, _u8L("Checking the firmware files"));
    if (!validate_package(pkg, board.short_name, err)) {
        fail(ctl, err);
        return;
    }

    // Reduce the release to what the user actually asked for. A release carries images for every
    // component - mainboard, WiFi, and both displays - and all four are almost never wanted at
    // once: only one display is physically present, and the WiFi module may not exist at all.
    //
    // The IAP is never filtered here. It is not an M997 target; it is the programmer the mainboard
    // flash runs from, and step 7 skips it when assembling the module list.
    {
        const FirmwareRole display = selection.display;
        const bool         wifi    = selection.wifi && board.has_wifi_module();
        auto it = std::remove_if(pkg.images.begin(), pkg.images.end(),
                                 [&](const FirmwareImage &i) {
                                     switch (i.role) {
                                     case FirmwareRole::Iap:       return false;
                                     case FirmwareRole::Mainboard: return !selection.mainboard;
                                     case FirmwareRole::Wifi:      return !wifi;
                                     case FirmwareRole::PanelDue:
                                     case FirmwareRole::PanelOta:  return i.role != display;
                                     default:                      return true;
                                     }
                                 });
        pkg.images.erase(it, pkg.images.end());
    }
    // An IAP on its own is not something to flash - it only exists to serve a mainboard image.
    if (!pkg.find(FirmwareRole::Mainboard)) {
        auto it = std::remove_if(pkg.images.begin(), pkg.images.end(),
                                 [](const FirmwareImage &i) { return i.role == FirmwareRole::Iap; });
        pkg.images.erase(it, pkg.images.end());
    }
    if (pkg.images.empty()) {
        fail(ctl, _u8L("Nothing was selected to update."));
        return;
    }

    // The IAP must be on the card before M997 or the command fails outright, and we cannot know
    // whether the copy already there matches this firmware, so it is always uploaded when known.
    if (!pkg.find(FirmwareRole::Iap) && !board.iap_filename.empty() && pkg.find(FirmwareRole::Mainboard))
        BOOST_LOG_TRIVIAL(warning) << "FirmwareUpdateJob: no IAP image in the package; relying on the copy "
                                      "already present on the SD card";

    if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

    // ---- 6. Upload ---------------------------------------------------------------------------
    // Upload order matters: the IAP goes first so that, if anything fails part way, the board is
    // never left with new firmware and a stale programmer.
    std::sort(pkg.images.begin(), pkg.images.end(), [](const FirmwareImage &a, const FirmwareImage &b) {
        auto rank = [](FirmwareRole r) {
            switch (r) {
            case FirmwareRole::Iap:       return 0;
            case FirmwareRole::Mainboard: return 1;
            case FirmwareRole::Wifi:      return 2;
            case FirmwareRole::PanelDue:  return 3;
            case FirmwareRole::PanelOta:  return 3;
            default:                      return 4;
            }
        };
        return rank(a.role) < rank(b.role);
    });

    const uint64_t total_bytes = std::max<uint64_t>(pkg.total_bytes(), 1);
    uint64_t       sent_bytes  = 0;

    for (const auto &img : pkg.images) {
        if (ctl.was_canceled()) { m_result.outcome = Outcome::Cancelled; return; }

        const std::string remote = "0:/firmware/" + img.filename;
        const uint64_t    base   = sent_bytes;
        update_status(ctl, int(45 + 40.0 * double(base) / double(total_bytes)),
                      (boost::format(_u8L("Uploading %1%")) % img.filename).str());

        auto on_progress = [&](uint64_t sent, uint64_t, bool &cancel) {
            if (ctl.was_canceled()) { cancel = true; return; }
            // Uploads occupy 45..85% of the bar, weighted by total byte count.
            const int pct = int(45 + 40.0 * double(base + sent) / double(total_bytes));
            update_status(ctl, std::min(pct, 85), (boost::format(_u8L("Uploading %1%")) % img.filename).str());
        };

        if (!client.upload(remote, img.data, on_progress, err)) {
            if (ctl.was_canceled() || err == "Cancelled") { m_result.outcome = Outcome::Cancelled; return; }
            // Nothing has been flashed yet, so the printer is still in a good state.
            fail(ctl, (boost::format(_u8L("Could not upload %1%: %2%. The printer has not been modified."))
                       % img.filename % err).str());
            return;
        }
        sent_bytes += img.data.size();
    }

    if (ctl.was_canceled()) {
        // Uploaded files are inert until M997 runs, so cancelling here is safe.
        m_result.outcome = Outcome::Cancelled;
        return;
    }

    // ---- 7. Flash ------------------------------------------------------------------------------
    // One M997 with every module, ascending and colon separated, the way DWC issues it.
    std::vector<int> modules;
    for (const auto &img : pkg.images) {
        const int m = firmware_role_to_module(img.role);
        if (m >= 0)
            modules.push_back(m);
    }
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
    if (modules.empty()) {
        fail(ctl, _u8L("There is nothing to flash for this printer."));
        return;
    }

    std::string s_param;
    for (size_t i = 0; i < modules.size(); ++i)
        s_param += (i ? ":" : "") + std::to_string(modules[i]);

    // A display image on the card may not carry the name RRF expects (PanelDueFirmware.bin for
    // module 4, unlayered-panel.bin for module 5, both compile-time constants in Configuration.h),
    // so name it explicitly when we can. RRF rejects P outright for more than one module -
    // "Filename can only be provided when updating exactly one module" - so this is only possible
    // on a display-only flash; a combined flash relies on the manifest using RRF's own names.
    std::string cmd = "M997 S" + s_param;
    if (modules.size() == 1 && (modules.front() == 4 || modules.front() == 5)) {
        const auto *img = modules.front() == 4 ? pkg.find(FirmwareRole::PanelDue)
                                               : pkg.find(FirmwareRole::PanelOta);
        if (img)
            cmd += " P\"" + img->filename + "\"";
    }

    // Only a mainboard image reboots the board: RRF's flashing1 state runs every auxiliary module
    // to completion first and only then enters flashing2, which is the one that restarts. A
    // display- or WiFi-only flash therefore never drops the connection, and must not be waited on
    // as though it would.
    const bool reboots = pkg.find(FirmwareRole::Mainboard) != nullptr;

    update_status(ctl, 87, _u8L("Starting the firmware update"));
    BOOST_LOG_TRIVIAL(info) << "FirmwareUpdateJob: sending " << cmd;
    if (!client.gcode(cmd, err)) {
        fail(ctl, (boost::format(_u8L("Could not start the firmware update: %1%")) % err).str());
        return;
    }

    // ---- 8. Wait for the board to come back -----------------------------------------------------
    // From here on the flash is underway and cancelling would not stop it, so the cancel check is
    // deliberately only used to stop waiting.
    if (reboots) {
        update_status(ctl, 90, _u8L("The printer is installing the firmware and will restart"));

        auto should_cancel = [&ctl]() { return ctl.was_canceled(); };
        auto on_tick       = [&](int elapsed) {
            update_status(ctl, 90 + std::min(elapsed / 20, 5),
                          (boost::format(_u8L("Waiting for the printer to restart (%1%s)")) % elapsed).str());
        };

        if (!client.wait_for_reboot(REBOOT_TIMEOUT_SECONDS, should_cancel, on_tick, err)) {
            if (ctl.was_canceled()) {
                // Be explicit: the update is still happening on the board regardless of this dialog.
                m_result.outcome = Outcome::Cancelled;
                m_result.message = _u8L("Stopped waiting. The printer is still installing the firmware and "
                                        "should come back on its own.");
                return;
            }
            fail(ctl,
                 (boost::format(_u8L("The printer did not come back after %1% seconds.")) % REBOOT_TIMEOUT_SECONDS).str(),
                 /* recovery_hint */ true);
            return;
        }
    } else {
        update_status(ctl, 90, _u8L("The printer is installing the display firmware"));
    }

    // ---- 9. Verify -------------------------------------------------------------------------------
    // Being reachable is not the same as being finished: when several modules are flashed in one
    // M997 the board answers again while it is still programming the rest, and reports its old
    // firmware version until that completes. Wait out the "updating" state before comparing.
    update_status(ctl, 95, _u8L("Finishing the update"));
    for (int waited = 0; waited < 60; waited += 2) {
        std::string s;
        if (!client.read_status(s, err) || boost::algorithm::to_lower_copy(s) != "updating")
            break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (!reboots) {
        // There is nothing to verify. The board never restarted, so it still reports the firmware
        // it was already running, and neither display publishes a version that RRF can be asked
        // for. Getting here means RRF accepted the image and finished streaming it; a panel then
        // validates its own OTA slot and rolls back if the new image will not boot.
        m_result.outcome       = Outcome::Success;
        m_result.version_after = board.firmware_version;
        m_result.message       = _u8L("The display firmware was sent and installed. The display restarts "
                                      "itself - a PanelDue comes back with no splash screen, which is "
                                      "normal after an update and not a sign that it failed.");
        update_status(ctl, 100, m_result.message);
        client.disconnect();
        return;
    }

    update_status(ctl, 97, _u8L("Checking the installed firmware"));
    DuetClient::BoardInfo after;
    if (!client.read_board_info(after, err)) {
        // The board answered rr_connect, so it is alive; we just could not confirm the version.
        m_result.outcome       = Outcome::Success;
        m_result.version_after = "";
        m_result.message       = _u8L("The printer restarted, but its firmware version could not be confirmed. "
                                      "Check the printer's status page.");
        update_status(ctl, 100, m_result.message);
        client.disconnect();
        return;
    }

    m_result.version_after = after.firmware_version;

    const bool expected_known = !pkg.version.empty();
    if (expected_known && after.firmware_version != pkg.version) {
        fail(ctl, (boost::format(_u8L("The printer restarted but reports %1%, not the expected %2%. "
                                      "The update may not have been applied."))
                   % after.firmware_version % pkg.version).str());
        client.disconnect();
        return;
    }

    m_result.outcome = Outcome::Success;
    m_result.message = (boost::format(_u8L("The printer is now running %1%.")) % after.firmware_version).str();
    update_status(ctl, 100, m_result.message);
    client.disconnect();
}

void FirmwareUpdateJob::finalize(bool canceled, std::exception_ptr &e)
{
    if (canceled && m_result.outcome != Outcome::Failed)
        m_result.outcome = Outcome::Cancelled;

    if (e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            m_result.outcome = Outcome::Failed;
            m_result.message = (boost::format(_u8L("The firmware update failed unexpectedly: %1%")) % ex.what()).str();
            BOOST_LOG_TRIVIAL(error) << "FirmwareUpdateJob: unhandled exception: " << ex.what();
        }
        // Handled here; do not let the worker rethrow and surface a second, generic error dialog.
        e = nullptr;
    }

    // Publish before the event so the handler can rely on it being current.
    if (m_result_slot)
        *m_result_slot = m_result;

    if (m_event_handle && ui_alive()) {
        wxCommandEvent event(EVT_FIRMWARE_UPDATE_FINISHED);
        event.SetInt(int(m_result.outcome));
        event.SetString(wxString::FromUTF8(m_result.message.c_str()));
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
    }
}

}} // namespace Slic3r::GUI
