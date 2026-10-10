#include "libslic3r/libslic3r.h"
#include "UserManager.hpp"
#include "DeviceManager.hpp"
#include "NetworkAgent.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"

#include "DeviceCore/DevManager.h"


namespace Slic3r {

UserManager::UserManager(NetworkAgent* agent)
{
    m_agent = agent;
}

UserManager::~UserManager()
{
}

void UserManager::set_agent(NetworkAgent* agent)
{
    m_agent = agent;
}

int UserManager::parse_json(std::string payload)
{
    bool restored_json = false;
    json j;
    json j_pre = json::parse(payload);
    if (j_pre.empty()) {
        return -1;
    }

    //bind/unbind

    try {
        if (j_pre.contains("bind")) {
            if (j_pre["bind"].contains("command")) {

                //bind
                if (j_pre["bind"]["command"].get<std::string>() == "bind") {
                    std::string dev_id;
                    std::string result;

                    if (j_pre["bind"].contains("dev_id")) {
                        dev_id = j_pre["bind"]["dev_id"].get<std::string>();
                    }

                    if (j_pre["bind"].contains("result")) {
                        result = j_pre["bind"]["result"].get<std::string>();
                    }

                    if (result == "success") {
                        DeviceManager* dev = GUI::wxGetApp().getDeviceManager();
                        if (!dev) {return -1;}

                        // Unlayered: pin-code bind dialog removed with the Bambu account surface.
                        dev->update_user_machine_list_info(GUI::wxGetApp().get_printer_cloud_provider());
                        dev->set_selected_machine(dev_id);
                        return 0;
                    }
                }
            }
        }
    }
    catch (...){}

    return -1;
}

} // namespace Slic3r
