#ifndef __SysFilesUpdateJob_HPP__
#define __SysFilesUpdateJob_HPP__

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/event.h>
#include <wx/window.h>

#include "Job.hpp"
#include "slic3r/Utils/DuetClient.hpp"
#include "slic3r/Utils/SysFilesBundle.hpp"
#include "slic3r/Utils/SysFilesUpdater.hpp"

namespace Slic3r {
namespace GUI {

// Writes a set of configuration files to a Duet's SD card on a worker thread.
//
// The firmware updater's counterpart, and deliberately the same shape: query first so the dialog
// can say what is installed and what is on offer, then a second run that does the writing. The
// run itself - preflight, backup, writing, rollback, marker, restart - is run_sys_files_update()
// in Utils/SysFilesUpdater, which has no wx in it so tests drive the same code against a fake
// board; this class only carries it onto the worker thread and its progress back to the dialog.
class SysFilesUpdateJob : public Job
{
public:
    using Outcome       = SysFilesUpdateResult::Outcome;
    using FileInfo      = SysFilesUpdateResult::FileInfo;
    using Result        = SysFilesUpdateResult;
    using Options       = SysFilesUpdateOptions;
    using SourceFactory = SysFileSourceFactory;

    // client_factory builds the transport - HTTP or USB serial - on the worker thread.
    SysFilesUpdateJob(DuetClientFactory client_factory, SourceFactory source_factory, Options options);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &e) override;

    void set_event_handle(wxWindow *handle) { m_event_handle = handle; }
    const Result &result() const { return m_result; }

    // Guard shared with the owning window: a cancelled job cannot be interrupted mid-request, so
    // it may outlive its dialog. Cleared by the dialog, after which no events are posted.
    void set_ui_alive(std::shared_ptr<std::atomic<bool>> alive) { m_ui_alive = std::move(alive); }
    // The worker destroys the job as soon as it finishes; the full result is published here just
    // before the completion event goes out.
    void set_result_slot(std::shared_ptr<Result> slot) { m_result_slot = std::move(slot); }

    int status_range() const { return 100; }

private:
    DuetClientFactory m_client_factory;
    SourceFactory     m_source_factory;
    Options           m_options;

    wxWindow                          *m_event_handle = nullptr;
    Result                             m_result;
    std::shared_ptr<Result>            m_result_slot;
    std::shared_ptr<std::atomic<bool>> m_ui_alive;
    // curl calls back constantly during an upload; one wx event per callback makes the dialog
    // unresponsive, so progress is coalesced.
    int64_t                            m_last_post_ms = 0;

    bool ui_alive() const { return !m_ui_alive || m_ui_alive->load(); }

    void update_status(Ctl &ctl, int percent, const std::string &msg);
};

wxDECLARE_EVENT(EVT_SYS_FILES_UPDATE_MESSAGE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SYS_FILES_UPDATE_FINISHED, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif // __SysFilesUpdateJob_HPP__
