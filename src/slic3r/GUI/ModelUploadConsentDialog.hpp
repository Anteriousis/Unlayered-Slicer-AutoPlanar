#ifndef slic3r_GUI_ModelUploadConsentDialog_hpp_
#define slic3r_GUI_ModelUploadConsentDialog_hpp_

#include <wx/checkbox.h>
#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"

namespace Slic3r { namespace GUI {

// The question every model upload must ask first (0.1.6), for one model at a time: crash support,
// troubleshooting, feedback the person asked to send. Usage diagnostics consent never stands in for
// it. Shows exactly what would be uploaded, why, whether AI training is proposed (a separate box,
// unticked), who receives it and how long it is kept, with "Upload" and "Don't upload" side by side.
//
// In 0.1.6 the Universe tab's project export (WebAppBridge, slicer.exportProject) asks it; any other
// feature that uploads a model goes through it the same way:
//
//     auto auth = ModelUploadConsentDialog::ask(parent, request);
//     if (!auth.consume(request.file_id)) return; // declined, closed, or already used
//
class ModelUploadConsentDialog : public DPIDialog
{
public:
    // Never shows a request that leaves out what the person must be told; returns a refused
    // authorisation instead.
    static PrivacyConsent::ModelUploadAuthorization ask(wxWindow *parent, const PrivacyConsent::ModelUploadRequest &request);

protected:
    ModelUploadConsentDialog(wxWindow *parent, const PrivacyConsent::ModelUploadRequest &request);
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    wxCheckBox *m_training{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ModelUploadConsentDialog_hpp_
