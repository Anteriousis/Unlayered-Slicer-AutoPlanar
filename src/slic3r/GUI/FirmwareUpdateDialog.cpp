#include "FirmwareUpdateDialog.hpp"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/settings.h>
#include <wx/statbox.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp"

namespace Slic3r {
namespace GUI {

// Minimal ProgressIndicator that drives this dialog's gauge and status line. The Job/Worker
// framework requires one; BBLStatusBarSend would also do, but it brings its own buttons and
// layout that do not fit a dialog with its own controls.
class FirmwareProgressIndicator : public ProgressIndicator
{
public:
    FirmwareProgressIndicator(wxGauge *gauge, wxStaticText *status) : m_gauge(gauge), m_status(status) {}

    void clear_percent() override { if (m_gauge) m_gauge->SetValue(0); }
    void show_error_info(wxString, int, wxString, wxString) override {}
    void set_range(int range) override { m_range = range; if (m_gauge) m_gauge->SetRange(range); }
    void set_cancel_callback(CancelFn fn = CancelFn()) override { m_cancel = std::move(fn); }
    void set_progress(int pr) override { if (m_gauge) m_gauge->SetValue(std::min(pr, m_range)); }
    void set_status_text(const char *txt) override
    {
        if (m_status && txt)
            m_status->SetLabel(wxString::FromUTF8(txt));
    }
    int get_range() const override { return m_range; }

    void call_cancel() { if (m_cancel) m_cancel(); }

private:
    wxGauge      *m_gauge  = nullptr;
    wxStaticText *m_status = nullptr;
    int           m_range  = 100;
    CancelFn      m_cancel;
};

FirmwareUpdateDialog::FirmwareUpdateDialog(wxWindow *parent, DynamicPrintConfig *config)
    : DPIDialog(parent, wxID_ANY, _L("Printer Firmware"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    if (config) {
        m_host     = config->opt_string("print_host");
        // RepRapFirmware's password shares the generic print host key, as it does for uploads.
        m_password = config->opt_string("printhost_apikey");
    }

    build_ui();

    m_progress = std::make_shared<FirmwareProgressIndicator>(m_gauge, m_status);
    m_worker   = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_progress, "firmware_worker");
    m_ui_alive = std::make_shared<std::atomic<bool>>(true);

    Bind(EVT_FIRMWARE_UPDATE_MESSAGE, &FirmwareUpdateDialog::on_job_message, this);
    Bind(EVT_FIRMWARE_UPDATE_FINISHED, &FirmwareUpdateDialog::on_job_finished, this);
    Bind(wxEVT_CLOSE_WINDOW, &FirmwareUpdateDialog::on_close, this);

    wxGetApp().UpdateDlgDarkUI(this);
    start_query();
}

FirmwareUpdateDialog::~FirmwareUpdateDialog()
{
    // Belt and braces: if the dialog is torn down by some path other than dismiss()/on_close,
    // the job must still stop posting events to this window before the worker is joined below.
    if (m_ui_alive)
        *m_ui_alive = false;
}

void FirmwareUpdateDialog::build_ui()
{
    auto *main = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(8);

    // How this dialog reaches the printer. A machine with no address is still updatable over
    // USB, so this row comes before anything that describes the printer.
    m_connection = new DuetConnectionPicker(this, m_host, m_password);
    m_connection->on_changed([this]() {
        if (m_phase != Phase::Working)
            start_query();
    });
    main->Add(m_connection, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_printer_label = new wxStaticText(this, wxID_ANY,
        m_host.empty() ? _L("Printer on USB")
                       : wxString::Format(_L("Printer at %s"), wxString::FromUTF8(m_host.c_str())));
    auto font = m_printer_label->GetFont();
    font.MakeBold();
    m_printer_label->SetFont(font);
    main->Add(m_printer_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_current_label   = new wxStaticText(this, wxID_ANY, _L("Installed firmware: checking..."));
    m_available_label = new wxStaticText(this, wxID_ANY, _L("Available firmware: checking..."));
    main->Add(m_current_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    main->Add(m_available_label, 0, wxLEFT | wxRIGHT | wxTOP, pad / 2);

    m_notes = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                             wxSize(FromDIP(460), FromDIP(140)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP | wxBORDER_SIMPLE);
    main->Add(m_notes, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // What to update. A release carries images for the mainboard, the Wi-Fi module and both
    // displays, and only some of them apply to any one printer. Everything that does apply is
    // ticked by default, so the ordinary case - "update my printer" - is still one button press;
    // a user who wants a single component unticks the others.
    auto *components = new wxStaticBoxSizer(wxVERTICAL, this, _L("What to update"));
    auto *cbox       = components->GetStaticBox();

    m_mainboard_check = new wxCheckBox(cbox, wxID_ANY, _L("Printer firmware"));
    m_mainboard_check->SetValue(true);
    components->Add(m_mainboard_check, 0, wxALL, pad / 2);

    m_wifi_check = new wxCheckBox(cbox, wxID_ANY, _L("Wi-Fi module"));
    m_wifi_check->Hide(); // Only shown when the board reports a Wi-Fi module.
    components->Add(m_wifi_check, 0, wxALL, pad / 2);

    // The display row carries its own picker: the release ships a PanelDue image and a panel
    // image, only one of which belongs on this machine, and the printer cannot always say which.
    m_display_row    = new wxBoxSizer(wxHORIZONTAL);
    m_display_check  = new wxCheckBox(cbox, wxID_ANY, _L("Display"));
    m_display_choice = new wxChoice(cbox, wxID_ANY);
    m_display_row->Add(m_display_check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
    m_display_row->Add(m_display_choice, 0, wxALIGN_CENTER_VERTICAL);
    components->Add(m_display_row, 0, wxALL, pad / 2);

    m_display_note = new wxStaticText(cbox, wxID_ANY, wxEmptyString);
    m_display_note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    components->Add(m_display_note, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad / 2);

    // Hidden until the query says both that this printer has a display and that the release
    // carries an image for it.
    m_display_check->Hide();
    m_display_choice->Hide();
    m_display_note->Hide();

    main->Add(components, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto on_toggle = [this](wxCommandEvent &e) { refresh_update_button(); e.Skip(); };
    m_mainboard_check->Bind(wxEVT_CHECKBOX, on_toggle);
    m_wifi_check->Bind(wxEVT_CHECKBOX, on_toggle);
    m_display_check->Bind(wxEVT_CHECKBOX, on_toggle);

    m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(10)));
    main->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, _L("Contacting the printer..."));
    main->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_file_btn   = new wxButton(this, wxID_ANY, _L("Choose File..."));
    m_update_btn = new wxButton(this, wxID_ANY, _L("Update"));
    m_close_btn  = new wxButton(this, wxID_CANCEL, _L("Close"));
    m_update_btn->Enable(false);
    buttons->Add(m_file_btn, 0, wxRIGHT, pad);
    buttons->AddStretchSpacer();
    buttons->Add(m_update_btn, 0, wxRIGHT, pad);
    buttons->Add(m_close_btn, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(16));

    m_file_btn->Bind(wxEVT_BUTTON, &FirmwareUpdateDialog::on_choose_file, this);
    m_update_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        // A hand-picked file has no version to compare, so it is always treated as a forced flash.
        start_update(/* force */ !m_local_file.empty());
    });
    // Must be EndModal, not Close(): this dialog is run with ShowModal(), and Close() does not
    // break out of the modal event loop, so the window appears frozen shut.
    m_close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { dismiss(); });

    SetSizer(main);
    Layout();
    Fit();
    SetMinSize(GetSize());
    CentreOnParent();
}

FirmwareUpdateJob::SourceFactory FirmwareUpdateDialog::make_source_factory() const
{
    const std::string local = m_local_file;
    // Read on the UI thread and captured by value: the factory runs on the job's worker, where
    // touching wxGetApp() is not safe.
    const std::string api_base = wxGetApp().app_config->github_api_base();
    return [local, api_base](const DuetClient::BoardInfo &board) -> std::unique_ptr<FirmwareSource> {
        if (!local.empty())
            return std::make_unique<LocalFileFirmwareSource>(local, board.firmware_filename,
                                                             board.iap_filename, board.wifi_filename);

        // Prefer a published release, fall back to whatever shipped with this installation. The
        // fallback is what makes the updater work offline, and while no release exists yet.
        auto chain = std::make_unique<ChainedFirmwareSource>();

        GitHubReleaseFirmwareSource::Options opts;
        opts.api_base                = api_base;
        opts.board_firmware_filename = board.firmware_filename;
        opts.board_iap_filename      = board.iap_filename;
        opts.board_wifi_filename     = board.wifi_filename;
        opts.board_short_name        = board.short_name;
        chain->add(std::make_unique<GitHubReleaseFirmwareSource>(std::move(opts)));

        auto bundled = std::make_unique<BundledFirmwareSource>();
        if (bundled->available())
            chain->add(std::move(bundled));

        return chain;
    };
}

void FirmwareUpdateDialog::start_query()
{
    set_phase(Phase::Querying);
    m_job_result = std::make_shared<FirmwareUpdateJob::Result>();
    auto job = std::make_unique<FirmwareUpdateJob>(m_connection->make_client_factory(), make_source_factory(),
                                                   FirmwareUpdateJob::Selection{}, /* force */ false,
                                                   /* query_only */ true);
    job->set_event_handle(this);
    job->set_result_slot(m_job_result);
    job->set_ui_alive(m_ui_alive);
    replace_job(*m_worker, std::move(job));
}

FirmwareUpdateJob::Selection FirmwareUpdateDialog::current_selection() const
{
    FirmwareUpdateJob::Selection sel;
    sel.mainboard = m_mainboard_check->GetValue();
    sel.wifi      = m_wifi_check->IsShown() && m_wifi_check->GetValue();
    // Unknown means "leave the display alone", which is also what an unticked box means, so the
    // role is only read when the row is both visible and ticked.
    sel.display = FirmwareRole::Unknown;
    if (m_display_check->IsShown() && m_display_check->GetValue()) {
        const int idx = m_display_choice->GetSelection();
        if (idx >= 0 && size_t(idx) < m_display_roles.size())
            sel.display = m_display_roles[size_t(idx)];
    }
    return sel;
}

void FirmwareUpdateDialog::refresh_update_button()
{
    m_update_btn->Enable(m_phase == Phase::Ready && !current_selection().nothing());
}

void FirmwareUpdateDialog::start_update(bool force)
{
    if (m_host.empty()) {
        show_error(this, _L("This printer has no address configured."));
        return;
    }

    // Last chance to back out: this is the point after which the printer gets rewritten. Only a
    // mainboard flash restarts the board, so a display-only run must not promise one.
    const wxString body =
        current_selection().mainboard
            ? _L("The printer will restart and be unavailable for a minute or two.\n\n"
                 "Do not power the printer off while it is updating.\n\nUpdate the firmware now?")
            : _L("The printer will not restart, but do not power it off while the update is "
                 "running.\n\nUpdate now?");
    MessageDialog confirm(this, body, _L("Update Firmware"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;

    set_phase(Phase::Working);
    m_job_result = std::make_shared<FirmwareUpdateJob::Result>();
    auto job = std::make_unique<FirmwareUpdateJob>(m_connection->make_client_factory(), make_source_factory(),
                                                   current_selection(),
                                                   force, /* query_only */ false);
    job->set_event_handle(this);
    job->set_result_slot(m_job_result);
    job->set_ui_alive(m_ui_alive);
    replace_job(*m_worker, std::move(job));
}

void FirmwareUpdateDialog::set_phase(Phase phase)
{
    m_phase = phase;
    const bool busy = (phase == Phase::Querying || phase == Phase::Working);
    refresh_update_button();
    m_file_btn->Enable(!busy);
    if (m_connection)
        m_connection->set_enabled(!busy);
    // Closing mid-flash would not stop the board, so only offer it when nothing is in flight.
    m_close_btn->SetLabel(busy ? _L("Cancel") : _L("Close"));
    if (phase == Phase::Querying)
        m_gauge->Pulse();
    Layout();
}

void FirmwareUpdateDialog::on_choose_file(wxCommandEvent &)
{
    wxFileDialog dlg(this, _L("Select a firmware file"), wxEmptyString, wxEmptyString,
                     _L("Firmware files") + " (*.uf2;*.bin)|*.uf2;*.bin|" + _L("All files") + " (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    m_local_file = dlg.GetPath().ToUTF8().data();
    m_available_label->SetLabel(wxString::Format(_L("Selected file: %s"), dlg.GetFilename()));
    m_notes->SetValue(_L("A firmware file chosen by hand is flashed as-is. Make sure it is built for "
                         "this printer's board before continuing."));
    // A hand-picked file bypasses the version check entirely, so allow the update regardless of
    // what the query concluded - but only once we know which board we are talking to.
    if (m_board_known)
        set_phase(Phase::Ready);
    Layout();
}

void FirmwareUpdateDialog::on_job_message(wxCommandEvent &evt)
{
    if (m_status)
        m_status->SetLabel(evt.GetString());
    if (m_gauge && m_phase != Phase::Querying)
        m_gauge->SetValue(std::min(evt.GetInt(), 100));
}

void FirmwareUpdateDialog::on_job_finished(wxCommandEvent &evt)
{
    const auto     outcome   = static_cast<FirmwareUpdateJob::Outcome>(evt.GetInt());
    const wxString msg       = evt.GetString();
    // A failed opening query is reported in the status line only. Stacking a modal error box on
    // top of a dialog that has only just opened is noise, and it obscures the Close button.
    const bool     was_query = (m_phase == Phase::Querying);

    // The job is destroyed by the worker once it finishes; the full result was published into
    // this slot immediately before the event was posted.
    if (m_job_result && m_job_result->board_known) {
        m_board       = m_job_result->board;
        m_board_known = true;

        m_printer_label->SetLabel(wxString::Format(_L("%s at %s"),
            wxString::FromUTF8(m_board.name.c_str()), wxString::FromUTF8(m_host.c_str())));

        wxString installed = wxString::FromUTF8(m_board.firmware_version.c_str());
        if (!m_board.firmware_date.empty())
            installed += wxString::Format(" (%s)", wxString::FromUTF8(m_board.firmware_date.c_str()));
        m_current_label->SetLabel(wxString::Format(_L("Installed firmware: %s"), installed));

        // Only offer the Wi-Fi module when this board actually has one to flash.
        m_wifi_check->Show(m_board.has_wifi_module());
    }

    // Offer the display only when the printer has one we can name AND the release carries an image
    // for it. Both halves matter: a release cut before displays were bundled has neither image, and
    // a machine that never ran displayid.g cannot say which of the two it has.
    if (m_job_result && m_job_result->outcome == FirmwareUpdateJob::Outcome::QueryOk) {
        m_display = m_job_result->display;

        const auto &roles = m_job_result->available_roles;
        auto        has   = [&roles](FirmwareRole r) {
            return std::find(roles.begin(), roles.end(), r) != roles.end();
        };

        m_display_roles.clear();
        m_display_choice->Clear();
        if (has(FirmwareRole::PanelDue)) {
            m_display_roles.push_back(FirmwareRole::PanelDue);
            m_display_choice->Append(_L("PanelDue"));
        }
        if (has(FirmwareRole::PanelOta)) {
            m_display_roles.push_back(FirmwareRole::PanelOta);
            m_display_choice->Append(_L("Unlayered 4.3in panel"));
        }

        // Hide the row entirely when the printer states it has no display, rather than offering an
        // update for hardware that is not there.
        const bool offer = !m_display_roles.empty() && !m_display.none;
        m_display_check->Show(offer);
        m_display_choice->Show(offer);
        m_display_note->Show(offer);

        if (offer) {
            int idx = 0;
            for (size_t i = 0; i < m_display_roles.size(); ++i)
                if (m_display_roles[i] == m_display.role)
                    idx = int(i);
            m_display_choice->SetSelection(idx);
            // Ticked only when the printer actually said which display it has. Mis-targeting costs
            // a timeout rather than damage - a PanelDue ignores the panel's announce and the panel
            // ignores eraseAndReset - but it would still report success having flashed nothing.
            m_display_check->SetValue(m_display.certain);
            m_display_note->SetLabel(
                m_display.certain
                    ? wxString::Format(_L("Detected: %s"), wxString::FromUTF8(m_display.evidence.c_str()))
                    : wxString::Format(_L("Not detected - %s. Choose the display to update it anyway."),
                                       wxString::FromUTF8(m_display.evidence.c_str())));
        }
        Layout();
        Fit();
    }

    if (m_job_result && !m_job_result->available_version.empty() && m_local_file.empty()) {
        m_available_version = m_job_result->available_version;
        m_available_label->SetLabel(wxString::Format(_L("Available firmware: %s"),
            wxString::FromUTF8(m_available_version.c_str())));
    }
    if (m_job_result && !m_job_result->available_notes.empty() && m_local_file.empty())
        m_notes->SetValue(wxString::FromUTF8(m_job_result->available_notes.c_str()));

    switch (outcome) {
    case FirmwareUpdateJob::Outcome::QueryOk: {
        m_gauge->SetValue(0);
        set_phase(Phase::Ready);
        const bool same = !m_available_version.empty() &&
                          m_available_version == m_board.firmware_version;
        if (same) {
            m_update_btn->SetLabel(_L("Reinstall"));
            m_status->SetLabel(_L("The printer is already up to date."));
        } else if (!m_available_version.empty()) {
            m_status->SetLabel(_L("An update is available."));
        } else {
            m_status->SetLabel(_L("Choose a firmware file to install."));
        }
        break;
    }

    case FirmwareUpdateJob::Outcome::UpToDate:
        m_gauge->SetValue(100);
        set_phase(Phase::Finished);
        // Still allow a deliberate re-flash of the same version.
        m_update_btn->SetLabel(_L("Reinstall"));
        m_update_btn->Enable(true);
        m_status->SetLabel(msg);
        break;

    case FirmwareUpdateJob::Outcome::Success:
        m_gauge->SetValue(100);
        set_phase(Phase::Finished);
        m_status->SetLabel(msg);
        show_info(this, msg, _L("Firmware updated"));
        break;

    case FirmwareUpdateJob::Outcome::Cancelled:
        set_phase(Phase::Ready);
        m_status->SetLabel(msg.IsEmpty() ? _L("Cancelled") : msg);
        break;

    case FirmwareUpdateJob::Outcome::Failed:
    default: {
        set_phase(Phase::Ready);
        m_status->SetLabel(msg);
        if (was_query) {
            // Nothing was changed on the printer; let the user retry, pick a file, or close.
            m_gauge->SetValue(0);
            if (m_local_file.empty())
                m_update_btn->Enable(false);
            Layout();
            break;
        }
        wxString full = msg;
        if (m_job_result && m_job_result->needs_recovery_hint) {
            // The board may be sitting in its bootloader with no network stack, so point at the
            // recovery path that does not need one.
            full += "\n\n";
            full += _L("If the printer does not come back on its own, connect it to this computer "
                       "by USB and press its Reset button twice quickly. It will appear as a USB "
                       "drive; copy the firmware file onto that drive to recover it.");
        }
        show_error(this, full);
        break;
    }
    }
    Layout();
}

// Shuts the background job down and leaves the modal loop. Safe to call more than once.
void FirmwareUpdateDialog::dismiss()
{
    // Stop the job from touching this window: it posts progress events to the dialog, and the
    // worker cannot be interrupted mid-request, so it may still be alive after we are gone.
    if (m_ui_alive)
        *m_ui_alive = false;

    if (m_worker) {
        m_worker->cancel_all();
        // Give an in-flight request a moment to notice. Bounded, so a hung socket cannot wedge
        // the close; the worker's own destructor waits again before joining.
        m_worker->wait_for_idle(1500);
    }

    if (IsModal())
        EndModal(wxID_CANCEL);
    else
        Destroy();
}

void FirmwareUpdateDialog::on_close(wxCloseEvent &evt)
{
    // The window-manager close button and Escape both land here.
    if (m_ui_alive)
        *m_ui_alive = false;
    if (m_worker)
        m_worker->cancel_all();

    if (IsModal()) {
        EndModal(wxID_CANCEL);
        return; // Do not Skip(): the modal loop is already unwinding.
    }
    evt.Skip();
}

void FirmwareUpdateDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    Refresh();
}

}} // namespace Slic3r::GUI
