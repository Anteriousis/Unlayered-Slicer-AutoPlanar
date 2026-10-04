#ifndef slic3r_SysFilesUpdateDialog_hpp_
#define slic3r_SysFilesUpdateDialog_hpp_

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
#include "Jobs/SysFilesUpdateJob.hpp"
#include "Jobs/Worker.hpp"

class wxButton;
class wxCheckBox;
class wxCheckListBox;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Installs a set of configuration files - config.g, the homing and tool change macros, whatever
// else lives in 0:/sys - onto a Duet/RepRapFirmware printer.
//
// Same shape as FirmwareUpdateDialog: the printer is queried in the background as the dialog opens
// (never on the UI thread, or a printer that is switched off freezes the application), and the
// install itself runs as a job on this dialog's worker. What is different is that the user picks
// which files go, because unlike a firmware image a configuration package is a set of independent
// files and some of them - a height map, a probe offset - are specific to one machine.
class SysFilesUpdateDialog : public DPIDialog
{
public:
    // Reads print_host and printhost_apikey from the physical printer's configuration, the same
    // keys the Test button uses.
    SysFilesUpdateDialog(wxWindow *parent, DynamicPrintConfig *config);
    ~SysFilesUpdateDialog() override;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    enum class Phase { Querying, Ready, Working, Finished };

    std::string m_host;
    std::string m_password;

    Phase m_phase = Phase::Querying;
    // Set when the user picks a folder or a zip by hand; overrides the release source.
    std::string m_local_path;
    // The card already reports the version on offer, so the button says Reinstall and the version
    // check has to be bypassed - otherwise pressing it just says "already installed" again.
    bool m_same_version = false;
    // Card-relative paths, in the order they appear in the list box.
    std::vector<std::string> m_file_paths;
    // Identifies the printer whose file selection is remembered. Empty until the printer answers.
    std::string m_selection_key;

    // The per-file ticks are a setting, not a per-session chore: which files a given printer should
    // have written is a property of that machine. Stored as the set of paths to LEAVE OUT rather
    // than the set to install, so a file that appears in a later release is installed by default -
    // remembering the inclusions instead would silently skip anything new.
    void        load_selection();
    void        save_selection() const;
    std::string selection_key_for(const SysFilesUpdateJob::Result &result) const;

    DuetConnectionPicker *m_connection = nullptr;
    // Whether the selected connection can read files back, which is what a backup needs.
    bool                  m_backup_available = true;

    void update_backup_availability();

    wxStaticText   *m_printer_label   = nullptr;
    wxStaticText   *m_installed_label = nullptr;
    wxStaticText   *m_available_label = nullptr;
    wxTextCtrl     *m_notes           = nullptr;
    wxCheckListBox *m_files           = nullptr;
    wxButton       *m_all_btn         = nullptr;
    wxButton       *m_none_btn        = nullptr;
    wxCheckBox     *m_backup_check    = nullptr;
    wxCheckBox     *m_restart_check   = nullptr;
    wxGauge        *m_gauge           = nullptr;
    wxStaticText   *m_status          = nullptr;
    wxButton       *m_install_btn     = nullptr;
    wxButton       *m_folder_btn      = nullptr;
    wxButton       *m_zip_btn         = nullptr;
    wxButton       *m_close_btn       = nullptr;

    std::shared_ptr<ProgressIndicator>         m_progress;
    std::unique_ptr<Worker>                    m_worker;
    // Written by the running job just before it posts its completion event.
    std::shared_ptr<SysFilesUpdateJob::Result> m_job_result;
    // Cleared when this dialog goes away, so a job that outlives it stops posting events here.
    std::shared_ptr<std::atomic<bool>>         m_ui_alive;

    void build_ui();
    void start_query();
    void start_install(bool force);
    void set_phase(Phase phase);
    void dismiss();
    void populate_files(const SysFilesUpdateJob::Result &result);
    std::vector<std::string> checked_files() const;

    void on_job_message(wxCommandEvent &evt);
    void on_job_finished(wxCommandEvent &evt);
    void on_choose_folder(wxCommandEvent &evt);
    void on_choose_zip(wxCommandEvent &evt);
    void on_close(wxCloseEvent &evt);

    // The release source, or the hand-picked folder/zip when there is one.
    SysFilesUpdateJob::SourceFactory make_source_factory() const;
};

}} // namespace Slic3r::GUI

#endif // slic3r_SysFilesUpdateDialog_hpp_
