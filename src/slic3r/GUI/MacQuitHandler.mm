#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <boost/log/trivial.hpp>

#include "MacQuitHandler.h"

@interface UnlayeredQuitHandler : NSObject {
@public
    std::function<bool()> intercept;
}
- (void)handleQuitAppEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation UnlayeredQuitHandler
- (void)handleQuitAppEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    BOOST_LOG_TRIVIAL(info) << "Quit Apple event received";
    if (intercept && intercept())
        return;
    if ([NSApp modalWindow] != nil) {
        // Another modal dialog is up (not the wizard). Refuse, as macOS did before this handler:
        // -terminate: under a modal session runs wxEVT_QUERY_END_SESSION -- which closes and shuts
        // down the main frame -- and then AppKit does not terminate, leaving a half-closed app.
        BOOST_LOG_TRIVIAL(info) << "Quit refused: a modal dialog is open";
        NSBeep();
        return;
    }
    [NSApp terminate:nil];
}
@end

namespace Slic3r {
namespace GUI {

void register_mac_quit_handler(std::function<bool()> intercept)
{
    static UnlayeredQuitHandler *handler = nil;
    if (handler == nil)
        handler = [[UnlayeredQuitHandler alloc] init];
    handler->intercept = std::move(intercept);

    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:handler
            andSelector:@selector(handleQuitAppEvent:withReplyEvent:)
          forEventClass:kCoreEventClass
             andEventID:kAEQuitApplication];
}

void *add_mac_quit_key_monitor(std::function<void()> on_quit)
{
    // Local monitors see key events as -[NSApp sendEvent:] dispatches them, modal sessions included.
    auto cb = std::make_shared<std::function<void()>>(std::move(on_quit));
    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                       handler:^NSEvent *(NSEvent *e) {
        const NSEventModifierFlags mods = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        if (mods == NSEventModifierFlagCommand &&
            [e.charactersIgnoringModifiers caseInsensitiveCompare:@"q"] == NSOrderedSame) {
            BOOST_LOG_TRIVIAL(info) << "Cmd-Q while a modal dialog is up";
            (*cb)();
            return nil;
        }
        return e;
    }];
    return (void *) CFBridgingRetain(monitor); // retained under ARC and MRC alike
}

void remove_mac_event_monitor(void *token)
{
    if (token == nullptr)
        return;
    id monitor = CFBridgingRelease(token);
    [NSEvent removeMonitor:monitor];
}

void request_mac_terminate()
{
    // Called right after the wizard has closed. If some other modal dialog came up in between,
    // wait for it (see handleQuitAppEvent: -terminate: under a modal leaves a half-closed app);
    // the main queue is serviced in modal run-loop modes too. Gives up after ~10 s.
    static int waits = 0;
    if ([NSApp modalWindow] != nil && waits < 20) {
        ++waits;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            request_mac_terminate();
        });
        return;
    }
    if ([NSApp modalWindow] != nil) {
        BOOST_LOG_TRIVIAL(info) << "Quit after the wizard dropped: another modal dialog stayed open";
        waits = 0;
        return;
    }
    waits = 0;
    [NSApp terminate:nil];
}

} // namespace GUI
} // namespace Slic3r
