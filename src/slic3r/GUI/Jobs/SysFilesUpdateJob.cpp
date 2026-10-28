#include "SysFilesUpdateJob.hpp"

#include <chrono>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SYS_FILES_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SYS_FILES_UPDATE_FINISHED, wxCommandEvent);

SysFilesUpdateJob::SysFilesUpdateJob(DuetClientFactory client_factory, SourceFactory source_factory, Options options)
    : m_client_factory(std::move(client_factory))
    , m_source_factory(std::move(source_factory))
    , m_options(std::move(options))
{}

void SysFilesUpdateJob::update_status(Ctl &ctl, int percent, const std::string &msg)
{
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

void SysFilesUpdateJob::process(Ctl &ctl)
{
    m_result = Result();

    // Built here, on the worker thread: the client owns an OS handle (a socket or a serial port)
    // and is confined to the thread that created it.
    std::unique_ptr<DuetClient> client = m_client_factory ? m_client_factory() : nullptr;
    if (!client) {
        m_result.outcome = Outcome::Failed;
        m_result.message = _u8L("No connection to the printer was configured.");
        return;
    }

    struct JobCtl : SysFilesUpdateCtl
    {
        SysFilesUpdateJob &job;
        Ctl               &ctl;
        JobCtl(SysFilesUpdateJob &j, Ctl &c) : job(j), ctl(c) {}
        void status(int percent, const std::string &message) override { job.update_status(ctl, percent, message); }
        bool cancelled() const override { return ctl.was_canceled(); }
    } job_ctl(*this, ctl);

    run_sys_files_update(*client, m_source_factory, m_options, job_ctl, m_result);
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

    // Usage diagnostics, only when the person has allowed them (UsageTelemetry checks). Versions and
    // counts only: never the printer's address or name, the files or the error text.
    if (!m_options.query_only && m_result.outcome != Outcome::QueryOk) {
        const char *outcome = m_result.outcome == Outcome::Success  ? "success"
                            : m_result.outcome == Outcome::UpToDate ? "up_to_date"
                            : m_result.outcome == Outcome::Cancelled ? "cancelled"
                                                                     : "failed";
        UsageTelemetry::record("printer_config_update",
                               {{"to", m_result.available_version.empty() ? std::string("unknown") : m_result.available_version},
                                {"from", m_result.installed.known ? m_result.installed.version : std::string("unknown")},
                                {"outcome", outcome},
                                {"files", int(m_result.written.size())},
                                {"rolled_back", m_result.rolled_back},
                                {"firmware", m_result.board_known ? m_result.board.firmware_version : std::string("unknown")}});
    }

    if (m_event_handle && ui_alive()) {
        wxCommandEvent event(EVT_SYS_FILES_UPDATE_FINISHED);
        event.SetInt(int(m_result.outcome));
        event.SetString(wxString::FromUTF8(m_result.message.c_str()));
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
    }
}

}} // namespace Slic3r::GUI
