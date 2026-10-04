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

namespace Slic3r {
namespace GUI {

// Writes a set of configuration files to a Duet's SD card on a worker thread.
//
// The firmware updater's counterpart, and deliberately the same shape: query first so the dialog
// can say what is installed and what is on offer, then a second run that does the writing. The
// difference that matters is that nothing here is atomic - each file is a separate request - so
// the job backs up what it is about to replace and refuses to start while a job is printing.
class SysFilesUpdateJob : public Job
{
public:
    enum class Outcome { Success, UpToDate, Failed, Cancelled, QueryOk };

    // One entry of the package, for the dialog's file list. The payload stays in the job.
    struct FileInfo
    {
        std::string path;
        uint64_t    size = 0;
    };

    struct Result
    {
        Outcome     outcome = Outcome::Failed;
        std::string message;

        // Filled by every run, so the dialog can describe the printer.
        DuetClient::BoardInfo board;
        bool                     board_known = false;
        SysFilesInstalled        installed;

        // What the source offered.
        std::string           available_version;
        std::string           available_date;
        std::string           available_machine;
        std::string           available_notes;
        std::string           available_origin;
        std::vector<FileInfo> available_files;
        // path -> why it was left out of the package.
        std::vector<std::pair<std::string, std::string>> skipped;

        // Filled by an install run.
        std::vector<std::string> written;
        std::string              backup_dir;
        bool                     restarted = false;
    };

    using SourceFactory = std::function<std::unique_ptr<SysFileSource>()>;

    struct Options
    {
        // Empty means "everything in the package". Paths are card-relative, e.g. "sys/config.g".
        std::vector<std::string> selected;
        // Download each file that is about to be overwritten into the slicer's data directory
        // first. There is no undo on the card, so this is the only way back to a working config.
        bool backup = true;
        // Send M999 afterwards. config.g is only read at startup, so without this the printer
        // keeps running its old configuration until somebody power cycles it.
        bool restart = true;
        // Install even when the card already reports this version.
        bool force = false;
        // Connect, read what is installed and resolve what is available, then stop.
        bool query_only = false;
    };

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
    SourceFactory m_source_factory;
    Options       m_options;

    wxWindow                          *m_event_handle = nullptr;
    Result                             m_result;
    std::shared_ptr<Result>            m_result_slot;
    std::shared_ptr<std::atomic<bool>> m_ui_alive;
    // curl calls back constantly during an upload; one wx event per callback makes the dialog
    // unresponsive, so progress is coalesced.
    int64_t                            m_last_post_ms = 0;

    bool ui_alive() const { return !m_ui_alive || m_ui_alive->load(); }

    void update_status(Ctl &ctl, int percent, const std::string &msg);
    void fail(Ctl &ctl, const std::string &msg);
    // Downloads the current contents of the files about to be replaced. Returns false only when
    // the user asked for a backup and it could not be made.
    bool make_backup(Ctl &ctl, DuetClient &client, const SysFilePackage &pkg, std::string &error_out);
};

wxDECLARE_EVENT(EVT_SYS_FILES_UPDATE_MESSAGE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SYS_FILES_UPDATE_FINISHED, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif // __SysFilesUpdateJob_HPP__
