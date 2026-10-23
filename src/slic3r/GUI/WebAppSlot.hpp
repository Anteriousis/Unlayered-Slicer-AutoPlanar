#pragma once

namespace Slic3r {
namespace GUI {

// One embedded-web-application "slot": the AppConfig keys, environment overrides and seed file
// a WebAppPanel is configured from. The panel is written once and instantiated per slot, so a
// second tab is a second row here (plus its keys in AppConfig.hpp), not a second copy of the
// panel or of the bridge.
//
// Deliberately free of wx and of anything heavier than const char*: WebAppBridge, Tab and
// Preferences all need to name a slot without pulling the panel's headers in.
//
// The three slots, of which two ship switched on:
//   agent()    -- the second slot. Off in release builds (development edition only).
//   universe() -- the "Universe" tab: browse models and open one straight onto the plate, and
//                 publish what is on the plate back. Holds the export permission, not settings.
//   generic()  -- the "Web application" tab: application-agnostic, hosts whatever a developer
//                 points ORCA_WEB_APP_URL at (WEB_APP_BRIDGE.md). Ships off, with no address.
//
// A slot's shipped values are the default_* fields below rather than resources/web/webapp/*.json,
// because those files are excluded from the installer; see ensure_seeded in WebAppPanel.cpp.
struct WebAppSlot
{
    const char* id; // stable short name; used in log lines and to tell the slots apart

    // AppConfig keys. All user-editable; their defaults are in AppConfig.cpp.
    const char* key_enabled;
    const char* key_url;
    const char* key_title;
    const char* key_allow_local;    // slicer.openProject from a non-loopback origin
    const char* key_path_roots;     // ';'-separated roots slicer.openProject may read from
    const char* key_download_hosts; // ';'-separated extra hosts openProjectFromUrl may fetch from
    const char* key_extra_origins;  // ';'-separated extra https origins the tab may NAVIGATE to
    const char* key_seeded;         // set once the seed file has been applied
    const char* key_allow_settings;
    // Gate for the export family: exportProject / readExportChunk / discardExport. Its own key
    // rather than a reuse of key_allow_settings, so a tab can be allowed to publish what is on
    // the plate without also being allowed to rewrite the print settings, or the other way round.
    const char* key_allow_export;
    // Gate for slicer.installPrinterPreset: the page may offer a printer profile for the user to
    // install. Its own key again, because letting a site ADD a preset is a different consent from
    // letting it read settings or take the plate away -- and even with it on, every install waits
    // on a native confirmation the page cannot skip or pre-answer.
    const char* key_allow_install;
    // Records the last one-shot visibility change applied to this slot, in either direction.
    // Deliberately not defaulted in AppConfig, so "never applied" stays distinguishable from any
    // tag. The key is named *_disable_once for the configurations already carrying it.
    const char* key_disable_once;
    // Non-null to change this tab's visibility ONCE on a configuration an earlier release left
    // set differently. `enabled` cannot: it is first-seed-only, which is right for a flag the
    // user may have set themselves but useless for adding or withdrawing a feature. Applied a
    // single time per tag and recorded in key_disable_once, so the user's own choice afterwards
    // stands; changing the tag arms it again. Declared here rather than in the seed file because
    // the seed files are not shipped.
    const char* visibility_once;
    bool        visibility_once_enable; // what the tag above sets key_enabled to

    // The address and tab name this build ships with, written to the configuration when it holds
    // none. These live here, and not in seed_file, because the seed files are excluded from the
    // installer: on a customer machine the slot is the only source of defaults there is. nullptr
    // leaves the key untouched, which is how a slot ships with no address at all.
    const char* default_url;
    const char* default_title;
    // The bridge permissions this build ships with, applied on the same terms the seed file
    // applies them: settings on a first seed only (a bool has no unset state, so a stored false
    // cannot be told from the user having switched it off), export and install whenever the key
    // is absent (they are newer than the installations they have to reach).
    bool default_allow_settings;
    bool default_allow_export;
    bool default_allow_install;
    // Hosts this tab may download from beyond the page's own, used when the configuration
    // names none. A leading dot matches any sub-domain. It exists because object storage
    // hands out a host belonging to the deployment rather than to the product, so no fixed
    // name can be shipped -- and because there is no Preferences field for this any more.
    const char* download_hosts;

    // Development overrides, read from the environment and never written back to AppConfig.
    // A non-empty env_url also enables the tab regardless of key_enabled.
    const char* env_url;
    const char* env_title;

    // File name under resources/web/webapp/ holding the shipped defaults, applied once.
    const char* seed_file;

    // A URL an earlier build seeded that is corrected one-shot when found verbatim in key_url
    // (see ensure_seeded in WebAppPanel.cpp). nullptr for a slot with no such history.
    const char* legacy_url;

    // true:  on a theme toggle the panel re-themes the loaded page through the bridge
    //        ("slicer.themeChanged") instead of letting WebView::RecreateAll reload it.
    // false: the page is reloaded, as every other web view in the application is.
    bool live_theme;

    static const WebAppSlot& generic();
    static const WebAppSlot& agent();
    static const WebAppSlot& universe();
};

} // namespace GUI
} // namespace Slic3r
