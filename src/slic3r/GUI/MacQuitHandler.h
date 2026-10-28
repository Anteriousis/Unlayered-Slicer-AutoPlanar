#ifndef slic3r_GUI_MacQuitHandler_h_
#define slic3r_GUI_MacQuitHandler_h_

#include <functional>

namespace Slic3r {
namespace GUI {

// macOS: keep Quit (Cmd-Q, Dock > Quit, logout / shut down) working while the first-run setup
// wizard is up. Two things blocked it there:
//  - the Quit Apple event: wxWidgets' kAEQuitApplication handler never fires since the 3.3.2
//    upgrade (same regression as the deep-link handler, DeepLinkHandlerMac.h), so a Quit only
//    reached the app through AppKit's -terminate:, which AppKit holds back while a modal session
//    runs: the request was dropped and the user had to Force Quit;
//  - Cmd-Q: the menu bar is disabled under an app-modal dialog.
//
// register_mac_quit_handler() re-registers the Quit Apple event handler. `intercept` runs first;
// when it returns true the request is taken care of (the wizard is being closed and the quit is
// re-issued afterwards), otherwise the normal path runs (-[NSApp terminate:] -> wxWidgets'
// applicationShouldTerminate -> wxEVT_QUERY_END_SESSION).
void register_mac_quit_handler(std::function<bool()> intercept);

// While a modal dialog runs: call `on_quit` for Cmd-Q (the key event is swallowed). Returns a
// token for remove_mac_event_monitor().
void *add_mac_quit_key_monitor(std::function<void()> on_quit);
void  remove_mac_event_monitor(void *token);

// The normal quit path, as the Quit menu item / Apple event would take it.
void request_mac_terminate();

} // namespace GUI
} // namespace Slic3r

#endif
