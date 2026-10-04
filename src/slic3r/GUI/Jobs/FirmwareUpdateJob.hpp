#ifndef __FirmwareUpdateJob_HPP__
#define __FirmwareUpdateJob_HPP__

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/window.h>

#include "Job.hpp"
#include "slic3r/Utils/DuetClient.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp"

namespace Slic3r {
namespace GUI {

// Runs the whole firmware update against a Duet board on a worker thread.
//
// The sequence follows what DuetWebControl does, with the two safety gaps it leaves open closed:
// DWC never checks whether a print is running (and neither does RepRapFirmware, which simply
// waits for motion to drain and then abandons the job), and DWC waits for the board to come back
// forever rather than eventually giving up with an actionable message.
class FirmwareUpdateJob : public Job
{
public:
    enum class Outcome { Success, UpToDate, Failed, Cancelled, QueryOk };

    // What the user asked to flash. A release carries images for every component; this says which
    // of them are actually sent. Split out because "update the firmware" and "update the display"
    // are separate decisions once a release bundles both.
    struct Selection
    {
        bool mainboard = true;
        bool wifi      = false;
        // PanelDue or PanelOta to flash that display; Unknown to leave the display alone. Never
        // guessed here - the dialog resolves it from DisplayInfo or from the user.
        FirmwareRole display = FirmwareRole::Unknown;

        bool nothing() const { return !mainboard && !wifi && display == FirmwareRole::Unknown; }
    };

    struct Result
    {
        Outcome     outcome = Outcome::Failed;
        std::string message;
        std::string version_before;
        std::string version_after;
        // Set when the board never came back; the UI shows the USB recovery instructions.
        bool needs_recovery_hint = false;

        // Filled by a query run so the dialog can describe the printer and the pending update.
        DuetClient::BoardInfo board;
        bool                     board_known = false;
        std::string              available_version;
        std::string              available_notes;
        std::string              available_origin;
        // Which components this release actually carries, so the dialog offers only those.
        std::vector<FirmwareRole> available_roles;
        // Which display the printer says it has. Resolved on the query pass so the dialog can
        // pre-select the right image, or ask when the machine cannot say.
        DisplayInfo              display;
    };

    // The source cannot be built until the board has told us what it calls its own firmware
    // files, so the caller supplies a factory that is invoked once that is known.
    using SourceFactory = std::function<std::unique_ptr<FirmwareSource>(const DuetClient::BoardInfo &)>;

    // client_factory builds the transport - HTTP or USB serial - on the worker thread.
    FirmwareUpdateJob(DuetClientFactory client_factory,
                      SourceFactory source_factory,
                      Selection     selection,
                      bool          force,
                      // Connect, identify the board and resolve what is available, then stop
                      // without downloading or flashing anything.
                      bool          query_only);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &e) override;

    void set_event_handle(wxWindow *handle) { m_event_handle = handle; }
    const Result &result() const { return m_result; }

    // Guard shared with the owning window. A cancelled job cannot be interrupted mid-request, so
    // it can still be running after its dialog is gone; when this is cleared, no further events
    // are posted to that (now dangling) handle.
    void set_ui_alive(std::shared_ptr<std::atomic<bool>> alive) { m_ui_alive = std::move(alive); }

    // The job is destroyed by the worker as soon as it finishes, so a caller that wants the full
    // result rather than just the summary carried on the completion event hands in a slot here.
    // It is written before the completion event is posted.
    void set_result_slot(std::shared_ptr<Result> slot) { m_result_slot = std::move(slot); }

    int status_range() const { return 100; }

private:
    DuetClientFactory m_client_factory;
    SourceFactory m_source_factory;
    Selection     m_selection;
    // Flash even when the reported version already matches, and when the source cannot state a
    // version at all (a hand-picked local file).
    bool          m_force       = false;
    bool          m_query_only  = false;

    wxWindow                          *m_event_handle = nullptr;
    Result                             m_result;
    std::shared_ptr<Result>            m_result_slot;
    std::shared_ptr<std::atomic<bool>> m_ui_alive;
    // Throttles progress events: curl calls back very frequently during an upload, and one wx
    // event per callback floods the UI thread badly enough to make the dialog unresponsive.
    int64_t                            m_last_post_ms = 0;

    bool ui_alive() const { return !m_ui_alive || m_ui_alive->load(); }

    void update_status(Ctl &ctl, int percent, const std::string &msg);
    void fail(Ctl &ctl, const std::string &msg, bool recovery_hint = false);
};

wxDECLARE_EVENT(EVT_FIRMWARE_UPDATE_MESSAGE, wxCommandEvent);
wxDECLARE_EVENT(EVT_FIRMWARE_UPDATE_FINISHED, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif // __FirmwareUpdateJob_HPP__
