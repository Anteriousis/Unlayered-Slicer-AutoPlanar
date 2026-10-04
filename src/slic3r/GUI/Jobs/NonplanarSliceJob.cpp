#include "NonplanarSliceJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <thread>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {
// The protocol carries no progress figure, so the bar creeps toward a ceiling while polling and
// only the real steps (submit, download, done) move it decisively. Honest about being a guess:
// it never reaches 100 until the file is actually in hand.
const int NPS_PCT_SUBMITTED = 10;
const int NPS_PCT_POLL_CEIL = 85;
const int NPS_PCT_DOWNLOAD  = 90;

const int NPS_POLL_INTERVAL_MS = 1500;
// A slice can legitimately take minutes; this only stops a wedged service hanging forever.
const int NPS_POLL_TIMEOUT_S = 3600;
} // namespace

NonplanarSliceJob::NonplanarSliceJob(std::string service_url,
                                     fs::path    mesh,
                                     fs::path    settings,
                                     std::string printer,
                                     std::string process,
                                     std::string filament,
                                     fs::path    out_dir)
    : m_url(std::move(service_url))
    , m_mesh(std::move(mesh))
    , m_settings(std::move(settings))
    , m_printer(std::move(printer))
    , m_process(std::move(process))
    , m_filament(std::move(filament))
    , m_out_dir(std::move(out_dir))
{}

void NonplanarSliceJob::process(Ctl &ctl)
{
    ctl.update_status(0, _u8L("Sending the plate to the slicing service..."));

    const NonplanarSliceClient client(m_url);
    NonplanarSliceClient::Job  job;

    if (! client.submit(m_mesh, m_settings, m_printer, m_process, m_filament, job, m_error))
        return;
    if (ctl.was_canceled()) { m_canceled = true; return; }

    m_warnings = job.warnings;
    ctl.update_status(NPS_PCT_SUBMITTED, _u8L("Slicing..."));

    const auto  started  = std::chrono::steady_clock::now();
    int         pct      = NPS_PCT_SUBMITTED;
    std::string last_text;
    while (! job.finished()) {
        if (ctl.was_canceled()) { m_canceled = true; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(NPS_POLL_INTERVAL_MS));
        if (ctl.was_canceled()) { m_canceled = true; return; }

        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count() > NPS_POLL_TIMEOUT_S) {
            m_error = _u8L("The slicing service did not finish in time.");
            return;
        }
        if (! client.poll(job.id, job, m_error))
            return;
        if (! job.warnings.empty())
            m_warnings = job.warnings;

        // Creep, never arrive: the service tells us nothing about how far along it is.
        if (pct < NPS_PCT_POLL_CEIL)
            ++pct;
        const std::string text = job.status == NonplanarSliceClient::Status::Queued
                                     ? _u8L("Waiting for the slicing service...")
                                     : _u8L("Slicing...");
        if (text != last_text) {
            last_text = text;
            ctl.update_status(pct, text);
        } else {
            ctl.update_status(pct);
        }
    }

    if (job.status == NonplanarSliceClient::Status::Failed) {
        m_error = job.error.empty() ? _u8L("The slicing service could not slice this plate.")
                                    : job.error;
        return;
    }

    // A unique local name per job, always: the preview silently ignores a G-code file whose path
    // matches the one it is already showing, so re-slicing would appear to do nothing.
    std::string name = job.filename;
    if (name.empty())
        name = "nonplanar.gcode";
    m_gcode = m_out_dir / (job.id.substr(0, 8) + "-" + name);

    ctl.update_status(NPS_PCT_DOWNLOAD, _u8L("Downloading the G-code..."));
    if (! client.download_gcode(job.id, m_gcode, m_error,
                                [&ctl](uint64_t received, uint64_t total) {
                                    if (ctl.was_canceled())
                                        return false;
                                    if (total > 0) {
                                        const int span = 100 - NPS_PCT_DOWNLOAD;
                                        ctl.update_status(NPS_PCT_DOWNLOAD +
                                                          int(span * double(received) / double(total)));
                                    }
                                    return true;
                                })) {
        if (m_error == "cancelled") {
            m_canceled = true;
            m_error.clear();
        }
        m_gcode.clear();
        return;
    }
    ctl.update_status(100, _u8L("Opening the G-code..."));
}

void NonplanarSliceJob::finalize(bool canceled, std::exception_ptr &e)
{
    // The exported plate and settings were only ever couriers.
    boost::system::error_code ec;
    if (! m_mesh.empty())
        fs::remove(m_mesh, ec);
    if (! m_settings.empty())
        fs::remove(m_settings, ec);

    if (e) {
        // Let the worker rethrow: an exception here is a bug, not a service failure, and the
        // usual handler will report it rather than this job swallowing it.
        return;
    }

    NotificationManager *notify = wxGetApp().plater() ? wxGetApp().plater()->get_notification_manager()
                                                      : nullptr;
    if (canceled || m_canceled) {
        BOOST_LOG_TRIVIAL(info) << "NonplanarSliceJob: cancelled";
        return;
    }

    if (! m_error.empty() || m_gcode.empty()) {
        const std::string msg = m_error.empty() ? _u8L("Nonplanar slicing failed.") : m_error;
        BOOST_LOG_TRIVIAL(error) << "NonplanarSliceJob: " << msg;
        if (notify != nullptr)
            notify->push_notification(NotificationType::CustomNotification,
                                      NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                      _u8L("Nonplanar slicing") + ": " + msg);
        return;
    }

    if (notify != nullptr && ! m_warnings.empty())
        notify->push_notification(NotificationType::CustomNotification,
                                  NotificationManager::NotificationLevel::WarningNotificationLevel,
                                  _u8L("Nonplanar slicing") + ": " +
                                      boost::algorithm::join(m_warnings, " · "));

    // Opens in G-code preview mode and selects the Preview tab itself. From there the result can
    // be printed or exported like any sliced plate (Plater::send_loaded_gcode / export_loaded_gcode).
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        plater->load_gcode(from_path(m_gcode));
}

}} // namespace Slic3r::GUI
