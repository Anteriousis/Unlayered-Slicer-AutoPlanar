#ifndef slic3r_DuetClient_hpp_
#define slic3r_DuetClient_hpp_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/property_tree/ptree_fwd.hpp>

namespace Slic3r {

// One way of talking to a Duet board running RepRapFirmware in standalone mode.
//
// There are two: DuetRrfClient over the rr_* HTTP API, and DuetSerialClient over the board's USB
// serial port. The firmware and config updaters are written against this interface so that neither
// of them has to know which cable the bytes are going down.
//
// Deliberately not a PrintHost subclass: PrintHost is shaped around "upload one G-code file and
// maybe print it", and updating a printer needs the object model, arbitrary G-code, CRC-checked
// binary uploads and the ability to wait out a reboot.
//
// THREADING. One instance belongs to the thread that created it and nothing here is thread safe.
// Both jobs construct their client inside process(), on the worker thread, and own it for the whole
// of that call - which is also why a client is built through a DuetClientFactory rather than handed
// across from the UI thread.
class DuetClient
{
public:
    // Result of a connection attempt. Mirrors the "err" field rr_connect returns; a transport with
    // no notion of sessions or passwords only ever answers ok or unreachable.
    enum class ConnectResult { ok, wrong_password, no_free_session, unreachable };

    struct BoardInfo
    {
        std::string firmware_version;    // boards[0].firmwareVersion, e.g. "3.7.0-beta.1+unlayered.1"
        std::string firmware_date;       // boards[0].firmwareDate
        std::string firmware_name;       // boards[0].firmwareName
        std::string firmware_filename;   // boards[0].firmwareFileName, e.g. "Duet3Firmware_Mini5plus.uf2"
        std::string iap_filename;        // boards[0].iapFileNameSD, e.g. "Duet3_SDiap32_Mini5plus.bin"
        std::string wifi_filename;       // boards[0].wifiFirmwareFileName; empty on Ethernet boards
        std::string short_name;          // boards[0].shortName, e.g. "Mini5plus"
        // boards[0].uniqueId, e.g. "W7W5M-KA6AA-Q65J0-401L8-K602Z-Z7245". The only genuinely
        // per-machine identifier a Duet offers: it survives a rename, an address change and a
        // move between the network and USB. Empty on firmware that does not report it.
        std::string unique_id;
        std::string name;                // boards[0].name, e.g. "Duet 3 Mini 5+"
        // network.name, i.e. what M550 set. Every Unlayered machine reports the same board name,
        // so this is the only thing that tells two of them apart in the UI.
        std::string machine_name;

        // network.interfaces[] contains an entry of type "wifi".
        //
        // This cannot be inferred from wifi_filename: a Mini 5+ runs one binary on both the WiFi
        // and the Ethernet variant and reports a wifiFirmwareFileName either way, so an Ethernet
        // board advertises a WiFi image it has no module to flash. Verified against two live
        // Ethernet boards, both of which reported "DuetWiFiServer.bin".
        bool wifi_interface = false;

        // True when the board reports a WiFi module whose firmware can be flashed (module 1).
        bool has_wifi_module() const { return wifi_interface && !wifi_filename.empty(); }
    };

    // One entry of a directory listing.
    struct FileEntry
    {
        std::string name;
        uint64_t    size   = 0;
        std::string date;   // "YYYY-MM-DDTHH:MM:SS" as reported by the board; empty when unset
        bool        is_dir = false;
    };

    // What this transport can actually do, so that a gap becomes a decision at the call site
    // instead of a mysterious failure inside a driver.
    struct Capabilities
    {
        // download() can return a file's contents at all. False on a board whose firmware predates
        // M1763, where there is no faithful way to read a file over a G-code channel.
        bool read_files = true;
        // ...and returns them byte for byte. Nothing sets this false today; it exists because the
        // stock M36.2 fallback would be text-only, and a backup that silently differs from the card
        // is worse than no backup.
        bool read_files_exact = true;
        // wait_for_reboot() returns to the same endpoint. False over USB, where a firmware update
        // can change the port's number, its PID and how many ports the board even has.
        bool stable_endpoint = true;
    };

    // Progress for a single upload. Mirrors Http::Progress but without the curl types leaking out.
    using ProgressFn = std::function<void(uint64_t sent, uint64_t total, bool &cancel)>;

    virtual ~DuetClient() = default;

    virtual Capabilities       capabilities() const = 0;
    // What to call this connection in a message to the user: "192.168.1.60" or "COM3".
    virtual const std::string &endpoint() const = 0;

    virtual ConnectResult connect(std::string &error_out) = 0;
    virtual void          disconnect() = 0;

    // The object model subtree at <key>, as rr_model / M409 return it.
    virtual bool object_model(const std::string &key, const std::string &flags,
                              boost::property_tree::ptree &result_out, std::string &error_out) = 0;

    virtual bool gcode(const std::string &cmd, std::string &error_out) = 0;

    // Uploads a binary blob to an absolute SD path (e.g. "0:/firmware/Duet3Firmware_Mini5plus.uf2").
    // The payload is sent verbatim and a CRC-32 always accompanies it, so the board validates the
    // transfer itself rather than trusting the link.
    virtual bool upload(const std::string &remote_path, const std::vector<uint8_t> &data,
                        ProgressFn progress, std::string &error_out) = 0;

    // missing_out distinguishes "the board answered, there is no such file" - a normal answer for an
    // optional file - from a transport failure. Callers rely on that difference: it is what tells
    // the config updater "nothing to back up" from "the read failed".
    virtual bool download(const std::string &remote_path, std::vector<uint8_t> &out,
                          bool &missing_out, std::string &error_out) = 0;

    // missing_out is set when the directory does not exist.
    virtual bool filelist(const std::string &dir, std::vector<FileEntry> &out,
                          bool &missing_out, std::string &error_out) = 0;

    // A false return is ADVISORY: RRF cannot distinguish "already exists" from a real failure on
    // either transport, so let the upload that follows be the real test.
    virtual bool mkdir(const std::string &dir, std::string &error_out) = 0;

    // Deletes a file. Used to clear up an upload that failed its CRC check - an image that did not
    // arrive intact must never be left in 0:/firmware where a later M997 could pick it up.
    virtual bool remove(const std::string &remote_path, std::string &error_out) = 0;

    // Waits out a reboot, e.g. after M997 or M999. Returns true once the board answers again.
    virtual bool wait_for_reboot(int timeout_seconds,
                                 const std::function<bool()> &should_cancel,
                                 const std::function<void(int elapsed_seconds)> &on_tick,
                                 std::string &error_out) = 0;

    // Non-virtual on purpose: pure ptree -> struct parsing over object_model(), and both transports
    // read the same object model. Duplicating these is the one place where a field could silently
    // come out different over USB than over the network.
    bool read_board_info(BoardInfo &info_out, std::string &error_out);
    // state.status, e.g. "idle", "processing", "paused", "updating".
    bool read_status(std::string &status_out, std::string &error_out);

    // Standard CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR) - the variant RRF checks
    // against, whether it was given as an rr_upload query parameter or as M559's C parameter.
    //
    // Not named crc32: miniz.h does `#define crc32 mz_crc32`, which would rewrite the name of this
    // member wherever that header has been included.
    static uint32_t compute_crc32(const uint8_t *data, size_t len);

    // Reads a file into memory in binary mode. Never use std::ifstream in text mode for firmware:
    // on Windows that collapses CRLF pairs and stops at 0x1A, silently corrupting the image.
    static bool read_file_binary(const std::string &path, std::vector<uint8_t> &out, std::string &error_out);
};

// Builds the transport for one run of a job. Called on the job's worker thread, so it must capture
// values only - never a wx object - exactly as the existing source factories do.
using DuetClientFactory = std::function<std::unique_ptr<DuetClient>()>;

} // namespace Slic3r

#endif // slic3r_DuetClient_hpp_
