#include "ModelUploadConsentDialog.hpp"

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PrivacySetupDialog.hpp"

namespace Slic3r { namespace GUI {

ModelUploadConsentDialog::ModelUploadConsentDialog(wxWindow *parent, const PrivacyConsent::ModelUploadRequest &q)
    : DPIDialog(parent, wxID_ANY, _L("Upload this model?"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU)
{
    SetBackgroundColour(*wxWHITE);
    auto *top  = new wxBoxSizer(wxVERTICAL);
    auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1, 1);

    const int wrap = FromDIP(420);
    auto row = [&](const wxString &label, const std::string &value) {
        auto *l = new wxStaticText(this, wxID_ANY, label);
        l->SetFont(l->GetFont().Bold());
        auto *v = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(value));
        v->Wrap(wrap);
        grid->Add(l, 0, wxALIGN_TOP);
        grid->Add(v, 1, wxEXPAND);
    };
    row(_L("File"), q.file_label);
    row(_L("What is uploaded"), q.included_data);
    row(_L("Why"), q.purpose);
    row(_L("Who receives it"), q.recipients);
    row(_L("How long it is kept"), q.retention);
    row(_L("AI training"), q.ai_training_proposed ? _u8L("Only if you tick the box below.") : _u8L("No. This upload is not used to train AI."));

    auto *intro = new wxStaticText(this, wxID_ANY,
                                   _L("Unlayered asks before uploading any model. This question is about this one file only; "
                                      "your usage diagnostics choice does not cover it."));
    intro->Wrap(wrap + FromDIP(120));
    top->Add(intro, 0, wxALL, FromDIP(16));
    top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    if (q.ai_training_proposed) {
        m_training = new wxCheckBox(this, wxID_ANY, _L("Also allow Unlayered to use this model to train and evaluate AI systems"));
        m_training->SetValue(false);
        top->Add(m_training, 0, wxALL, FromDIP(16));
    }

    auto *policy = new wxHyperlinkCtrl(this, wxID_ANY, _L("Read Full Privacy Policy"), "file:///" + PrivacyFlow::policy_file());
    policy->Bind(wxEVT_HYPERLINK, [](wxHyperlinkEvent &) { PrivacyFlow::open_policy(); });
    top->Add(policy, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // Same size, same style, side by side: declining is as easy as agreeing.
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *no      = new wxButton(this, wxID_NO, _L("Don't upload"));
    auto *yes     = new wxButton(this, wxID_YES, _L("Upload"));
    const wxSize size(FromDIP(140), -1);
    no->SetMinSize(size);
    yes->SetMinSize(size);
    buttons->AddStretchSpacer();
    buttons->Add(no, 0, wxRIGHT, FromDIP(8));
    buttons->Add(yes, 0);
    top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(16));
    no->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_NO); });
    yes->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_YES); });
    SetEscapeId(wxID_NO);
    // No default button: Enter must not upload.
    SetSizerAndFit(top);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

PrivacyConsent::ModelUploadAuthorization ModelUploadConsentDialog::ask(wxWindow *parent, const PrivacyConsent::ModelUploadRequest &q)
{
    if (!PrivacyConsent::request_complete(q)) {
        BOOST_LOG_TRIVIAL(error) << "privacy: a model upload asked without saying what, why, to whom and for how long; refused";
        return {};
    }
    ModelUploadConsentDialog dlg(parent, q);
    if (dlg.ShowModal() != wxID_YES)
        return {};
    return PrivacyConsent::ModelUploadAuthorization::granted(q, dlg.m_training && dlg.m_training->GetValue());
}

}} // namespace Slic3r::GUI
