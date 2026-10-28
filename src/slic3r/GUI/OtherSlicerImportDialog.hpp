#ifndef slic3r_GUI_OtherSlicerImportDialog_hpp_
#define slic3r_GUI_OtherSlicerImportDialog_hpp_

#include <functional>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/OtherSlicerImport.hpp"

class wxBoxSizer;
class wxButton;
class wxCheckBox;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class GUI_App;

// Unlayered 0.1.7: "Import your settings from other slicers". The work is OtherSlicers (libslic3r);
// this is where it meets the preset bundle and the person. Reached from File > Import, from the
// setup wizard's import step (guide 5import, before the printer picker), and once after an update
// (ask_import_other_slicers_once). All three run import_other_slicers() and show its summary.

// What the other slicers on this machine hold that has not been imported yet.
struct OtherSlicersFound
{
    std::vector<OtherSlicers::Scan>   scans;  // every app with a data directory
    std::vector<OtherSlicers::Counts> counts; // per scan: what is not imported yet

    int total() const;
    // "OrcaSlicer, Bambu Studio": the apps with something to import.
    wxString apps() const;
    // "Bambu Studio" or "OrcaSlicer" when its printer list has Bambu Lab printers (vendor "BBL"),
    // whether or not they were imported already; "" otherwise. The setup wizard then ticks the
    // Bambu Lab network plug-in box.
    wxString bambu_printers_in() const;
    // "6 printers, 40 filaments, 3 custom presets"
    static wxString describe(const OtherSlicers::Counts &counts);
};

// Looks for OrcaSlicer, PrusaSlicer and Bambu Studio and reads their user presets and the printers
// and filaments picked from their built-in lists. Quick: nothing is resolved yet.
OtherSlicersFound find_other_slicers();

// What one import did.
struct OtherSlicersImportResult
{
    int      presets{0};   // the person's own presets imported
    int      printers{0};  // printer model + nozzle variants enabled from the built-in lists
    int      filaments{0}; // system filaments enabled from the built-in lists
    wxString selected_printer; // the printer now selected (a fresh install took the app's), or ""
    // Per app: what came over, what was skipped and why, API keys to enter again. Shown to the person.
    wxString summary;
    int      total() const { return presets + printers + filaments; }
};

// Imports from the given apps now: enables the built-in printers and filaments (installing their
// vendor profiles), selects the app's current printer on a fresh install, imports the user presets
// and printer addresses, records it all in the import history and reloads the presets. Never fails
// as a whole: whatever cannot come over is listed in the summary. `parent`: for error messages.
OtherSlicersImportResult import_other_slicers(wxWindow *parent, const std::vector<OtherSlicers::Scan> &selected);

class OtherSlicerImportDialog : public DPIDialog
{
public:
    OtherSlicerImportDialog(wxWindow *parent, OtherSlicersFound found, bool start_now);

    // Shows the dialog. `start_now`: import from every app at once (the person already said yes,
    // in the setup wizard or the question after an update). True when presets were imported.
    static bool run(wxWindow *parent, bool start_now = false);
    static bool run(wxWindow *parent, OtherSlicersFound found, bool start_now);

    bool imported_any() const { return m_imported > 0; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void do_import();

    OtherSlicersFound        m_found;
    std::vector<wxCheckBox *> m_checks; // per scan, null for an app with nothing new
    wxBoxSizer              *m_choice_sizer{nullptr};
    wxTextCtrl              *m_summary{nullptr};
    wxButton                *m_import{nullptr};
    wxButton                *m_close{nullptr};
    int                      m_imported{0};
};

// On the first launch after an update: if another slicer has something that was never imported,
// asks once whether to import it (Import / Not now / Don't ask again), then imports and shows the
// summary. After the setup wizard, which offers the same thing before its printer picker, it only
// records that the question was asked. "Not now" asks again after the next update if there is
// still something new.
void ask_import_other_slicers_once(GUI_App &app, bool wizard_showed);

// Runs `fn` once the app has started and no other dialog (setup wizard, What's new, the privacy
// question, another startup question) is open, so two questions never stack.
void run_when_no_dialog(GUI_App &app, std::function<void()> fn);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_OtherSlicerImportDialog_hpp_
