#ifndef slic3r_GUI_DuetConnectionPicker_hpp_
#define slic3r_GUI_DuetConnectionPicker_hpp_

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "slic3r/Utils/DuetClient.hpp"

class wxChoice;
class wxStaticText;

namespace Slic3r { namespace GUI {

// A "Network / USB" row for the Firmware and Config dialogs.
//
// It owns no connection. It answers one question - how should this dialog talk to the printer -
// and says when that answer changed so the dialog can re-run its query. Shared by both dialogs so
// the two cannot drift apart.
class DuetConnectionPicker : public wxPanel
{
public:
    // host may be empty: a printer with no address is still updatable over USB, which is the whole
    // point of this control.
    DuetConnectionPicker(wxWindow *parent, std::string host, std::string password);

    // Re-enumerates and re-picks a default. Call after a firmware update: the port number, the USB
    // product id and even the number of ports the board presents can all have changed, so pass the
    // topology key to keep the same physical board selected.
    void refresh(const std::string &prefer_topology_key = std::string());

    bool        is_usb() const;
    std::string port() const;
    std::string topology_key() const;

    // True when there is anything at all to talk to - an address, or a Duet on USB.
    bool has_connection() const { return !m_entries.empty(); }

    // What the selected transport can do, without opening it. Used to disable the backup option
    // when the connection cannot read files back.
    DuetClient::Capabilities selected_capabilities() const;

    // Builds the factory the job will use. This is the single place in the application that decides
    // between HTTP and USB.
    DuetClientFactory make_client_factory() const;

    void on_changed(std::function<void()> fn) { m_on_changed = std::move(fn); }

    // Greyed out while a job is running - changing transport mid-update is not a thing.
    void set_enabled(bool enabled);

private:
    struct Entry
    {
        bool        usb = false;
        std::string port;            // USB only
        std::string topology_key;    // USB only
        bool        usable = true;   // false for a port whose driver failed to start
    };

    std::string        m_host;
    std::string        m_password;
    std::vector<Entry> m_entries;

    wxChoice     *m_choice = nullptr;
    wxStaticText *m_note   = nullptr;

    std::function<void()> m_on_changed;

    void update_note();
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DuetConnectionPicker_hpp_
