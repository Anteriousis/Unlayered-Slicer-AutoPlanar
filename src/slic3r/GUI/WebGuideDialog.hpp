#ifndef slic3r_WebGuideDialog_hpp_
#define slic3r_WebGuideDialog_hpp_

#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/frame.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"

#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/PresetUpdater.hpp"

#include <unordered_map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

class GuideFrame : public DPIDialog
{
public:
    GuideFrame(GUI_App *pGUI, long style = wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU);
    virtual ~GuideFrame();

    enum GuidePage {
        BBL_WELCOME,
        BBL_REGION,
        BBL_MODELS,
        BBL_FILAMENTS,
        BBL_FILAMENT_ONLY,
        BBL_MODELS_ONLY
    }m_page;

    //Web Function
    void load_url(wxString &url);
    wxString SetStartPage(GuidePage startpage=BBL_WELCOME, bool load = true);

    void UpdateState();
    void OnIdle(wxIdleEvent &evt);
    // void OnClose(wxCloseEvent &evt);

    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnNewWindow(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnTitleChanged(wxWebViewEvent &evt);
    void OnFullScreenChanged(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    void OnScriptResponseMessage(wxCommandEvent &evt);
    void RunScript(const wxString &javascript);

    //Logic
    bool IsFirstUse();

    //Model - Machine - Filaments
    // Orca/Unlayered: profile data loads in two stages so the printer picker does not have to wait
    // for every vendor's machine/filament/process presets. Models is ~450 files, Details is ~11500.
    enum class ProfileStage { Models, Details };

    int LoadProfileData();
    int SaveProfileData();
    int LoadProfileFamily(std::string strVendor, std::string strFilePath, ProfileStage stage, json &out);
    // Builds and sends the response_userguide_profile message. UI thread only.
    void SendProfileToWeb();
    // Marks filaments already installed per app_config as selected in m_ProfileJson.
    void ApplyInstalledFilamentSelection();
    // Merges stage-2 data into m_ProfileJson. UI thread, idempotent; false if not ready yet.
    bool MergeDetailData();
    int SaveProfile();
    int GetFilamentInfo( std::string VendorDirectory,json & pFilaList, std::string filepath, std::string &sVendor, std::string &sType);


    bool apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdater *updater, bool& apply_keeped_changes);
    bool run();

    void        StrReplace(std::string &strBase, std::string strSrc, std::string strDes);
    std::string w2s(wxString sSrc);
    void        GetStardardFilePath(std::string &FilePath);
    bool LoadFile(std::string jPath, std::string & sContent);

    // install plugin
    int DownloadPlugin();
    int InstallPlugin();
    int ShowPluginStatus(int status, int percent, bool &cancel);

    void on_dpi_changed(const wxRect &suggested_rect) {}

private:
    GUI_App *m_MainPtr;
    AppConfig m_appconfig_new;

    wxWebView *m_browser;
    wxButton * m_TestBtn;

    wxString m_SectionName;

    bool orca_bundle_rsrc;
    boost::filesystem::path vendor_dir;
    boost::filesystem::path rsrc_vendor_dir;

    //First Load
    bool bFirstComplete{false};
    bool m_destroy{false};
    boost::thread* m_load_task{ nullptr };

    // User Config
    bool PrivacyUse;
    bool StealthMode;
    std::string m_Region;

    bool InstallNetplugin;
    bool network_plugin_ready {false};

    json m_OrcaFilaList;
    std::string m_OrcaFilaLibPath;

    // Stage-2 (machine/filament/process) load state. The loader publishes into m_detail_json under
    // m_detail_mutex; the UI thread merges it into m_ProfileJson either from the posted CallAfter
    // and never blocks the UI thread, so a page that needs filaments is never handed an
    // empty list. m_detail_loaded and m_detail_request_pending are UI thread only.
    json       m_detail_json;
    std::mutex m_detail_mutex;
    bool       m_detail_ready{false}; // guarded by m_detail_mutex
    bool       m_detail_loaded{false};
    bool       m_detail_request_pending{false};
    // Default materials named by a printer picked before the filament list existed.
    std::set<std::string> m_pending_default_materials;

#if wxUSE_WEBVIEW_IE
    wxMenuItem *m_script_object_el;
    wxMenuItem *m_script_date_el;
    wxMenuItem *m_script_array_el;
#endif
    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    wxString m_bbl_user_agent;
    std::string m_editing_filament_id;

    struct CachedFilamentInfo
    {
        int         status{-1};
        std::string vendor;
        std::string type;
    };
    std::unordered_map<std::string, CachedFilamentInfo> filament_info_cache;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_Tab_hpp_ */
