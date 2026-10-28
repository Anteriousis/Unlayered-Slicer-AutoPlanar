#ifndef slic3r_GUI_PrinterUpdateOffer_hpp_
#define slic3r_GUI_PrinterUpdateOffer_hpp_

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "slic3r/Utils/PrinterUpdateCheck.hpp"

class wxWindow;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

class GUI_App;

// The credential the printer-update feeds are given, read on the UI thread: the Unlayered account's
// device token when signed in, otherwise nothing. It is bound to the account's site, so it can only
// ever reach that host (FeedCredential::token_for). The feed accepts nothing else - the old pasted
// "printer_updates_token" is no longer read. Shared by the Config dialog and the offer below, so
// the two cannot disagree about who is asking.
FeedCredential printer_feed_credential();

// "Printer update available": tells a customer, unprompted, that their Unlayered printer has newer
// firmware or configuration waiting, and installs both in the safe order with one click.
//
// WHEN. Sidebar::update_all_preset_comboboxes() is where the application decides how to reach the
// selected printer - it is what points the Device tab at the printer's own web page - and it runs
// at start-up and whenever the printer or its connection settings change. That is the moment the
// slicer learns which physical printer it is talking to, so it calls on_printer_selected() there,
// and nowhere else. Nothing polls: a Duet has about five HTTP sockets, so a printer gets ONE short
// read per session (connect, boards[0], network, 0:/sys/unlayered-sys.json, disconnect). A printer
// that does not answer may be retried on a later selection, but not within RETRY_AFTER.
//
// WHAT. On a worker thread: the board read, then one firmware-feed query (the release list and its
// manifest.json), then - signed in, and not in stealth mode - one configuration-feed query (the
// release list and its sys.zip, because the configuration's version is the one inside the
// archive's manifest, not the free-form tag). decide_printer_update() compares; anything newer
// and not skipped becomes a notification with Update / Not now / Skip this version. "Update" opens
// the Firmware dialog first (when the firmware is newer), then the Config dialog, each of which
// starts its install as soon as its own query confirms there is something to do. The config
// dialog's minFirmware gate is what makes that order safe: a configuration that needs the newer
// firmware is refused until the firmware is actually on the board.
//
// Customers get this; it is not fenced to the developer edition.
class PrinterUpdateOffer
{
public:
    explicit PrinterUpdateOffer(GUI_App &app);
    ~PrinterUpdateOffer();

    // The printer preset's configuration (host_type, print_host, printhost_apikey). Ignored unless
    // host_type is Unlayered and there is an address; cheap when the printer was already checked.
    void on_printer_selected(const DynamicPrintConfig &printer_config);

    // The sequenced flow behind "Update": the Firmware dialog when `firmware`, then the Config dialog
    // when `config`, both modal, both starting their install as soon as they are ready.
    static void run_update_flow(wxWindow *parent, const std::string &host, const std::string &password,
                                bool firmware, bool config);

    static constexpr std::chrono::minutes RETRY_AFTER{10};

private:
    struct Request
    {
        std::string    host;
        std::string    password;
        std::string    preset_name;
        std::string    api_base;
        bool           ask_config = false;
        FeedCredential credential;
    };
    struct Answer;

    static Answer check(const Request &request);
    void          on_checked(const Request &request, const Answer &answer);
    PrinterUpdateSkips skips() const;

    GUI_App                           &m_app;
    std::shared_ptr<std::atomic<bool>> m_alive;
    // Hosts checked (or being checked) this session, and boards already offered - a board can be
    // reachable at two addresses, and uniqueId is what identifies the machine.
    std::set<std::string>                                           m_hosts;
    std::set<std::string>                                           m_boards;
    std::map<std::string, std::chrono::steady_clock::time_point>    m_retry_at;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PrinterUpdateOffer_hpp_
