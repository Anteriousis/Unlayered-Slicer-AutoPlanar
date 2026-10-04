#include "DuetConnectionPicker.hpp"

#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/DuetSerialClient.hpp"
#include "slic3r/Utils/Serial.hpp"

namespace Slic3r { namespace GUI {

DuetConnectionPicker::DuetConnectionPicker(wxWindow *parent, std::string host, std::string password)
    : wxPanel(parent, wxID_ANY), m_host(std::move(host)), m_password(std::move(password))
{
    SetBackgroundColour(parent->GetBackgroundColour());

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = new wxStaticText(this, wxID_ANY, _L("Connection:"));
    row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_choice = new wxChoice(this, wxID_ANY);
    row->Add(m_choice, 1, wxALIGN_CENTER_VERTICAL);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(row, 0, wxEXPAND);

    m_note = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_note->SetForegroundColour(wxColour(0x80, 0x80, 0x80));
    outer->Add(m_note, 0, wxTOP, FromDIP(2));

    SetSizer(outer);

    m_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &evt) {
        evt.Skip();
        update_note();
        if (m_on_changed)
            m_on_changed();
    });

    refresh();
}

void DuetConnectionPicker::refresh(const std::string &prefer_topology_key)
{
    m_entries.clear();
    m_choice->Clear();

    if (!m_host.empty()) {
        Entry e;
        e.usb = false;
        m_entries.push_back(e);
        m_choice->Append(wxString::Format(_L("Network - %s"), wxString::FromUTF8(m_host.c_str())));
    }

    int prefer_index = -1;
    for (const Utils::SerialPortInfo &p : Utils::scan_duet_ports()) {
        Entry e;
        e.usb          = true;
        e.port         = p.port;
        e.topology_key = p.topology_key;
        e.usable       = (p.problem_code == 0);

        wxString text;
        if (!e.usable) {
            // A device whose driver failed to start is still enumerated and still has a port name,
            // so without saying why, it looks exactly like a working printer that will not answer.
            text = wxString::Format(_L("USB - %s (Windows cannot start this device - unplug it and plug it back in)"),
                                    wxString::FromUTF8(p.port.c_str()));
        } else {
            text = wxString::Format(_L("USB - %s"), wxString::FromUTF8(p.port.c_str()));
        }
        if (e.usable && !prefer_topology_key.empty() && p.topology_key == prefer_topology_key)
            prefer_index = int(m_entries.size());

        m_entries.push_back(e);
        m_choice->Append(text);
    }

    if (m_entries.empty()) {
        m_choice->Append(_L("No printer address and no Duet on USB"));
        m_choice->SetSelection(0);
        m_choice->Enable(false);
        update_note();
        return;
    }

    m_choice->Enable(true);

    int select = prefer_index;
    if (select < 0) {
        // Prefer the network when there is an address - it is what the user configured - and fall
        // back to the first healthy USB port otherwise.
        if (!m_host.empty()) {
            select = 0;
        } else {
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].usable) { select = int(i); break; }
            if (select < 0)
                select = 0;
        }
    }
    m_choice->SetSelection(select);
    update_note();
}

void DuetConnectionPicker::update_note()
{
    if (!m_note)
        return;
    wxString text;
    if (m_entries.empty()) {
        text = _L("Enter the printer's address, or connect it to this computer with a USB cable.");
    } else if (is_usb()) {
        // Worth saying plainly: over USB the password field on the printer is meaningless, and
        // people do go looking for why it is ignored.
        text = _L("Over USB no password is needed. The printer does not have to be on the network.");
    }
    m_note->SetLabel(text);
    m_note->Show(!text.IsEmpty());
    if (GetSizer())
        GetSizer()->Layout();
}

bool DuetConnectionPicker::is_usb() const
{
    const int sel = m_choice ? m_choice->GetSelection() : wxNOT_FOUND;
    return sel >= 0 && size_t(sel) < m_entries.size() && m_entries[sel].usb;
}

std::string DuetConnectionPicker::port() const
{
    const int sel = m_choice ? m_choice->GetSelection() : wxNOT_FOUND;
    return (sel >= 0 && size_t(sel) < m_entries.size()) ? m_entries[sel].port : std::string();
}

std::string DuetConnectionPicker::topology_key() const
{
    const int sel = m_choice ? m_choice->GetSelection() : wxNOT_FOUND;
    return (sel >= 0 && size_t(sel) < m_entries.size()) ? m_entries[sel].topology_key : std::string();
}

DuetClient::Capabilities DuetConnectionPicker::selected_capabilities() const
{
    DuetClient::Capabilities caps;
    if (is_usb()) {
        // Pessimistic until a connection is actually made: whether files can be read back depends
        // on the firmware the board is running, which is not known until it is asked.
        caps.read_files       = false;
        caps.read_files_exact = false;
        caps.stable_endpoint  = false;
    }
    return caps;
}

DuetClientFactory DuetConnectionPicker::make_client_factory() const
{
    // Everything is captured by value: the factory runs on a worker thread, long after this control
    // may have gone away.
    if (is_usb()) {
        const std::string p = port();
        const std::string k = topology_key();
        return [p, k]() -> std::unique_ptr<DuetClient> {
            return std::make_unique<DuetSerialClient>(p, k);
        };
    }
    const std::string h  = m_host;
    const std::string pw = m_password;
    return [h, pw]() -> std::unique_ptr<DuetClient> {
        return std::make_unique<DuetRrfClient>(h, pw);
    };
}

void DuetConnectionPicker::set_enabled(bool enabled)
{
    if (m_choice)
        m_choice->Enable(enabled && !m_entries.empty());
}

}} // namespace Slic3r::GUI
