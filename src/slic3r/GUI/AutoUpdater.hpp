#ifndef slic3r_GUI_AutoUpdater_hpp_
#define slic3r_GUI_AutoUpdater_hpp_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <boost/filesystem/path.hpp>

#include "slic3r/Utils/AppUpdater.hpp"
#include "slic3r/Utils/UnlayeredAccount.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

class GUI_App;

// The account-based self-update ("Restart to update").
//
// When the update manifest offers an installer behind the Unlayered account (its
// `unlayered_downloads`), and this copy is a per-user Windows install that can replace itself, the
// update happens with no installer wizard at all:
//
//   1. download in the background with the account's device token, verify the sha256;
//   2. a notification says the version is ready - "Restart to update";
//   3. restarting, or simply closing the application later, runs the installer silently once this
//      process has exited, and a restart brings the new version straight back up.
//
// Signed out, the update dialog offers "Sign in and Update" instead, which runs the browser sign-in
// (UnlayeredAccount) and then step 1. Anything this cannot handle - another platform, a per-machine
// install, a portable copy, no account download in the manifest - is left to the classic dialog,
// unchanged.
class AutoUpdater
{
public:
    explicit AutoUpdater(GUI_App &app);
    ~AutoUpdater();

    UnlayeredAccount &account() { return *m_account; }

    // Once, after the main window exists: loads the stored sign-in, re-checks it with the site in the
    // background, sweeps old downloads and announces a just-finished update.
    void on_startup();

    // Once per version, after the startup wizard: on the first launch of a newer version - however it
    // was installed - shows that version's CHANGELOG.md sections in a "What's new" dialog, like
    // OrcaSlicer's release-note view. A first run (`first_run`, the setup wizard showed) only records
    // the version, so a fresh install never opens with a changelog.
    void show_whats_new(bool first_run);

    // From the "new version" event. Returns true when it has taken the update over, false to let the
    // classic dialog run. `skipped` is whether the person chose "Skip this Version" for it before.
    bool handle_new_version(const std::string &version, const std::string &notes, const std::string &page_url,
                            const std::optional<AccountDownload> &download, bool by_user, bool skipped);

    // Help menu actions.
    void sign_in(wxWindow *parent, std::function<void(bool signed_in)> done = {});
    void sign_out();

    // From GUI_App::OnExit: starts the silent install of a downloaded update, if there is one. The
    // installer waits for this process to exit before it touches anything.
    void on_app_exit();

    bool update_ready() const { return m_state == State::Ready; }
    const std::string &ready_version() const { return m_version; }
    // Closes the application (with the usual save prompts) and installs + relaunches.
    void restart_to_update();

private:
    enum class State { Idle, Downloading, Ready };

    void start_download();
    void on_download_finished(bool ok, const boost::filesystem::path &installer, const std::string &error,
                              unsigned http_status);
    void notify_ready();
    void notify(const std::string &text, const std::string &link = {}, std::function<void()> on_link = {});

    GUI_App                          &m_app;
    // Shared, not unique: worker threads hold a reference so a request still in flight at exit
    // never outlives the object it writes to.
    std::shared_ptr<UnlayeredAccount> m_account;
    // Cleared in the destructor so a download thread finishing late never touches a dead object.
    std::shared_ptr<std::atomic<bool>> m_alive;

    State                          m_state{State::Idle};
    std::string                    m_version;
    AccountDownload                m_download;
    boost::filesystem::path        m_installer;
    std::optional<boost::filesystem::path> m_install_dir;
    bool                           m_relaunch{false};
    // UNLAYERED_MOD -- for the update_download diagnostics event: how long the download took.
    std::chrono::steady_clock::time_point m_download_started;
};

// The "## <version>" sections of a CHANGELOG.md that are newer than `after` and no newer than
// `up_to`, in file order, headings included. An empty `after` means the `up_to` section alone (an
// update from a version that predates this, so its own last-seen version is unknown). Every "## "
// heading ends a section, but only one starting with a version can be selected; returns "" when
// nothing matches.
std::string changelog_sections(const std::string &markdown, const std::string &after, const std::string &up_to);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AutoUpdater_hpp_
