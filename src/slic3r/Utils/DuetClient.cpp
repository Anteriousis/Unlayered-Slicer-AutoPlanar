#include "DuetClient.hpp"

#include <fstream>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/miniz_extension.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

uint32_t DuetClient::compute_crc32(const uint8_t *data, size_t len)
{
    // miniz's mz_crc32 is the standard zlib CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR),
    // which is the variant RRF's CRC32 class computes and compares against. It is also what M38
    // reports, so an upload can be verified independently of the transfer's own check.
    return static_cast<uint32_t>(::mz_crc32(MZ_CRC32_INIT, data, len));
}

bool DuetClient::read_file_binary(const std::string &path, std::vector<uint8_t> &out, std::string &error_out)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error_out = (boost::format("Could not open file: %1%") % path).str();
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        error_out = (boost::format("Could not determine size of file: %1%") % path).str();
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char *>(out.data()), size)) {
        error_out = (boost::format("Could not read file: %1%") % path).str();
        return false;
    }
    return true;
}

bool DuetClient::read_board_info(BoardInfo &info_out, std::string &error_out)
{
    pt::ptree board;
    // "v" asks for verbose fields; the firmware/IAP filenames live behind that flag.
    if (!object_model("boards[0]", "v", board, error_out))
        return false;

    info_out.firmware_version  = board.get<std::string>("firmwareVersion", "");
    info_out.firmware_date     = board.get<std::string>("firmwareDate", "");
    info_out.firmware_name     = board.get<std::string>("firmwareName", "");
    info_out.firmware_filename = board.get<std::string>("firmwareFileName", "");
    info_out.iap_filename      = board.get<std::string>("iapFileNameSD", "");
    info_out.wifi_filename     = board.get<std::string>("wifiFirmwareFileName", "");
    info_out.short_name        = board.get<std::string>("shortName", "");
    info_out.unique_id         = board.get<std::string>("uniqueId", "");
    info_out.name              = board.get<std::string>("name", "");

    if (info_out.firmware_version.empty() || info_out.firmware_filename.empty()) {
        error_out = "The board did not report its firmware version or firmware file name";
        return false;
    }

    // Decide whether there is a WiFi module to flash from the interfaces the board actually has,
    // not from wifiFirmwareFileName, which is populated on Ethernet boards too.
    pt::ptree network;
    std::string net_err;
    if (object_model("network", "v", network, net_err)) {
        // network.name is what M550 set, and is the only field that distinguishes two machines
        // built on the same board.
        info_out.machine_name = network.get<std::string>("name", "");
        if (auto ifaces = network.get_child_optional("interfaces")) {
            for (const auto &kv : *ifaces) {
                if (kv.second.get<std::string>("type", "") == "wifi") {
                    info_out.wifi_interface = true;
                    break;
                }
            }
        }
    } else {
        // Not fatal: without this we simply never offer the WiFi module.
        BOOST_LOG_TRIVIAL(warning) << "DuetClient: could not read network interfaces: " << net_err;
    }

    return true;
}

bool DuetClient::read_status(std::string &status_out, std::string &error_out)
{
    pt::ptree state;
    if (!object_model("state", "f", state, error_out))
        return false;
    status_out = state.get<std::string>("status", "");
    if (status_out.empty()) {
        error_out = "The board did not report a machine status";
        return false;
    }
    return true;
}

} // namespace Slic3r
