#include "MenuMnemonics.hpp"

#include <wx/menu.h>

#include <set>

namespace Slic3r { namespace GUI {

namespace {

// Label without mnemonic markers; a literal "&&" becomes a single '&'.
wxString strip_mnemonics(const wxString& label)
{
    wxString out;
    out.reserve(label.size());
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '&') {
            if (i + 1 < label.size() && label[i + 1] == '&') {
                out += '&';
                ++i;
            }
            continue;
        }
        out += label[i];
    }
    return out;
}

// Only ASCII letters and digits make dependable mnemonics across toolkits and keyboard layouts.
bool is_mnemonic_candidate(wxUniChar c) { return c.IsAscii() && std::isalnum(static_cast<unsigned char>(c.GetValue())); }

int mnemonic_key(wxUniChar c) { return wxTolower(static_cast<int>(c.GetValue())); }

} // namespace

void assign_menu_mnemonics(wxMenu* menu)
{
#ifndef __APPLE__
    if (menu == nullptr)
        return;

    std::set<int> used;
    for (wxMenuItem* item : menu->GetMenuItems()) {
        if (item->IsSeparator())
            continue;
        if (wxMenu* sub = item->GetSubMenu())
            assign_menu_mnemonics(sub);

        // Keep the "\tCtrl+X" accelerator part intact; only the visible label is rewritten.
        wxString       label = item->GetItemLabel();
        wxString       accel;
        if (const int tab = label.Find('\t'); tab != wxNOT_FOUND) {
            accel = label.Mid(tab);
            label = label.Left(tab);
        }
        label = strip_mnemonics(label);

        // Prefer the first letter of a word; fall back to any free letter.
        int pos = -1;
        for (size_t i = 0; i < label.size() && pos < 0; ++i)
            if (is_mnemonic_candidate(label[i]) && (i == 0 || label[i - 1] == ' ') && used.count(mnemonic_key(label[i])) == 0)
                pos = int(i);
        for (size_t i = 0; i < label.size() && pos < 0; ++i)
            if (is_mnemonic_candidate(label[i]) && used.count(mnemonic_key(label[i])) == 0)
                pos = int(i);

        wxString out;
        for (size_t i = 0; i < label.size(); ++i) {
            if (int(i) == pos)
                out += '&';
            if (label[i] == '&')
                out += "&&";
            else
                out += label[i];
        }
        if (pos >= 0)
            used.insert(mnemonic_key(label[pos]));

        out += accel;
        if (out != item->GetItemLabel())
            item->SetItemLabel(out);
    }
#endif
}

}} // namespace Slic3r::GUI
