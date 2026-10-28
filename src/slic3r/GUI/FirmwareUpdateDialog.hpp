#ifndef slic3r_FirmwareUpdateDialog_hpp_
#define slic3r_FirmwareUpdateDialog_hpp_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "DuetConnectionPicker.hpp"
#include "GUI_Utils.hpp"
#include "Jobs/FirmwareUpdateJob.hpp"
#include "Jobs/Worker.hpp"

class wxButton;
class wxCheckBox;
class wxChoice;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Firmware updater for Duet/RepRapFirmware based printers.
//
// Opens by querying the printer in the background (never on the UI thread - a printer that is off
// would otherwise freeze the application), then offers the update. The actual flash runs as a
// FirmwareUpdateJob on this dialog's own worker.
class FirmwareUpdateDialog : public DPIDialog
{
public:
    // Reads print_host and printhost_apikey from the physical printer's configuration, the same
    // keys the Test button uses.
    FirmwareUpdateDialog(wxWindow *parent, DynamicPrintConfig *config);
    ~FirmwareUpdateDialog() override;

    void on_dpi_changed(const wxRect &suggested_rect) override;

    // The "Printer update available" flow: start the update (with its usual confirmation) as soon
    // as the opening query finds a different version, and close once it has succeeded so the next
    // step of the flow can open.
    void begin_when_ready() { m_begin_when_ready = m_close_on_success = true; }

private:
    enum class Phase { Querying, Ready, Working, Finished };

    std::string m_host;
    std::string m_password;

    Phase m_phase = Phase::Querying;
    bool  m_begin_when_ready = false;
    bool  m_close_on_success = false;
    // Populated by the initial query and used to build the source for the real run.
    DuetClient::BoardInfo m_board;
    bool                     m_board_known = false;
    std::string              m_available_version;
    // Set when the user picks a file by hand; overrides the GitHub source.
    std::string              m_local_file;
    // What the printer said about its display, and which display images this release carries.
    DisplayInfo              m_display;
    // Parallel to the entries in m_display_choice, so a selection maps back to a role.
    std::vector<FirmwareRole> m_display_roles;

    DuetConnectionPicker *m_connection = nullptr;

    wxStaticText *m_printer_label   = nullptr;
    wxStaticText *m_current_label   = nullptr;
    wxStaticText *m_available_label = nullptr;
    wxTextCtrl   *m_notes           = nullptr;
    wxCheckBox   *m_mainboard_check = nullptr;
    wxCheckBox   *m_wifi_check      = nullptr;
    wxCheckBox   *m_display_check   = nullptr;
    wxChoice     *m_display_choice  = nullptr;
    wxStaticText *m_display_note    = nullptr;
    wxSizer      *m_display_row     = nullptr;
    wxCheckBox   *m_www_check       = nullptr;
    wxGauge      *m_gauge           = nullptr;
    wxStaticText *m_status          = nullptr;
    wxButton     *m_update_btn      = nullptr;
    wxButton     *m_file_btn        = nullptr;
    wxButton     *m_close_btn       = nullptr;

    std::shared_ptr<ProgressIndicator>              m_progress;
    std::unique_ptr<Worker>                         m_worker;
    // Written by the running job just before it posts its completion event.
    std::shared_ptr<FirmwareUpdateJob::Result>      m_job_result;
    // Cleared when this dialog goes away, so a job that outlives it stops posting events here.
    std::shared_ptr<std::atomic<bool>>              m_ui_alive;

    void build_ui();
    // Reads the component checkboxes into the selection the job takes. Also the single place that
    // decides what "display" means, since the choice control maps to a role.
    FirmwareUpdateJob::Selection current_selection() const;
    // Enables Update only when at least one component is selected, so the button never starts a
    // run that would immediately fail with "nothing was selected".
    void refresh_update_button();
    void start_query();
    void start_update(bool force);
    void set_phase(Phase phase);
    // Cancels the job and leaves the modal loop.
    void dismiss();
    void on_job_message(wxCommandEvent &evt);
    void on_job_finished(wxCommandEvent &evt);
    void on_choose_file(wxCommandEvent &evt);
    void on_close(wxCloseEvent &evt);

    // Builds the source used for a run: the hand-picked file when there is one, otherwise the
    // GitHub release, falling back to whatever shipped with the application.
    FirmwareUpdateJob::SourceFactory make_source_factory() const;
};

}} // namespace Slic3r::GUI

#endif // slic3r_FirmwareUpdateDialog_hpp_
