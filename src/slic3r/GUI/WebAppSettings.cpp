#include "WebAppSettings.hpp"

#include "PartPlate.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {
namespace WebAppSettings {

// ---------------------------------------------------------------------------------------------
// Deny lists. Adding to these is cheap and welcome; removing from them needs the argument made
// in the WebAppBridge.hpp header comment.
// ---------------------------------------------------------------------------------------------

const std::vector<std::string> WAB_SECRET_KEYS = {
    // The seven PresetBundle::full_config_secure() erases before a config leaves the process.
    "print_host",
    "print_host_webui",
    "printhost_apikey",
    "printhost_cafile",
    "printhost_user",
    "printhost_password",
    "printhost_port",
    // Same category: how, and whether, the slicer talks to a print host.
    "printhost_ssl_ignore_revoke",
    "printhost_authorization_type",
    "bbl_use_printhost",
    "host_type",
};

const std::vector<std::string> WAB_CODE_KEYS = {
    // The sixteen custom-G-code options of PrintConfig.cpp, in the order they are defined there.
    "before_layer_change_gcode",
    "machine_end_gcode",
    "printing_by_object_gcode",
    "filament_end_gcode",
    "layer_change_gcode",
    "time_lapse_gcode",
    "wrapping_detection_gcode",
    "machine_pause_gcode",
    "template_custom_gcode",
    "process_change_extrusion_role_gcode",
    "file_start_gcode",
    "machine_start_gcode",
    "filament_start_gcode",
    "change_filament_gcode",
    "change_extrusion_role_gcode",
    "filament_change_extrusion_role_gcode",
    // Post-processing scripts are commands the slicer runs after slicing.
    "post_process",
};

const std::vector<std::string> WAB_IDENTITY_KEYS = {
    "inherits",
    "printer_model",
    "printer_variant",
    "printer_technology",
    "compatible_prints",
    "compatible_printers",
    "compatible_prints_condition",
    "compatible_printers_condition",
    "filament_vendor",
    "filament_id",
};

namespace {

bool listed(const std::vector<std::string>& list, const std::string& key)
{
    return std::find(list.begin(), list.end(), key) != list.end();
}

} // namespace

bool key_is_secret(const std::string& key) { return listed(WAB_SECRET_KEYS, key); }
bool key_is_code(const std::string& key) { return listed(WAB_CODE_KEYS, key); }
bool key_is_identity(const std::string& key) { return listed(WAB_IDENTITY_KEYS, key); }

bool key_readable(const std::string& key, bool include_code)
{
    if (print_config_def.get(key) == nullptr)
        return false;
    if (key_is_secret(key))
        return false;
    if (!include_code && key_is_code(key))
        return false;
    return true;
}

bool key_writable(const std::string& key, std::string& why)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr) {
        why = "unknown option";
        return false;
    }
    if (key_is_secret(key)) {
        why = "print-host address, credential or upload switch: never exposed over the bridge";
        return false;
    }
    if (key_is_code(key)) {
        why = "custom G-code and post-processing scripts are never written over the bridge";
        return false;
    }
    if (key_is_identity(key) || boost::algorithm::ends_with(key, "_settings_id")) {
        why = "preset identity: select another preset with slicer.selectPreset instead";
        return false;
    }
    if (def->readonly) {
        why = "read-only option";
        return false;
    }
    if (def->is_plugin_backed()) {
        why = "plugin-backed option: configured through the slicer's plugin picker only";
        return false;
    }
    why.clear();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------

bool preset_type_from_string(const std::string& name, Preset::Type& type)
{
    if (name == "printer") {
        type = Preset::TYPE_PRINTER;
    } else if (name == "print") {
        type = Preset::TYPE_PRINT;
    } else if (name == "filament") {
        type = Preset::TYPE_FILAMENT;
    } else {
        type = Preset::TYPE_INVALID;
        return false;
    }
    return true;
}

const char* preset_type_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINTER:      return "printer";
    case Preset::TYPE_PRINT:        return "print";
    case Preset::TYPE_FILAMENT:     return "filament";
    case Preset::TYPE_SLA_PRINT:    return "sla_print";
    case Preset::TYPE_SLA_MATERIAL: return "sla_material";
    default:                        return "unknown";
    }
}

const char* type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat:            return "float";
    case coFloats:           return "floats";
    case coInt:              return "int";
    case coInts:             return "ints";
    case coString:           return "string";
    case coStrings:          return "strings";
    case coPercent:          return "percent";
    case coPercents:         return "percents";
    case coFloatOrPercent:   return "float_or_percent";
    case coFloatsOrPercents: return "floats_or_percents";
    case coPoint:            return "point";
    case coPoints:           return "points";
    case coPoint3:           return "point3";
    case coBool:             return "bool";
    case coBools:            return "bools";
    case coEnum:             return "enum";
    case coEnums:            return "enums";
    case coPointsGroups:     return "points_groups";
    case coIntsGroups:       return "ints_groups";
    default:                 return "none";
    }
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple:   return "simple";
    case comAdvanced: return "advanced";
    case comExpert:   return "expert";
    case comDevelop:  return "develop";
    default:          return "simple";
    }
}

// ---------------------------------------------------------------------------------------------
// JSON shapes
// ---------------------------------------------------------------------------------------------

json option_schema_json(const std::string& key, const ConfigBase* cfg)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr)
        return json();

    json option        = json::object();
    option["key"]      = key;
    option["type"]     = type_name(def->type);
    option["vector"]   = !def->is_scalar();
    option["nullable"] = def->nullable;
    // Untranslated source strings: a page localises with its own catalogue, or not at all.
    option["label"]     = def->label;
    option["fullLabel"] = def->full_label;
    option["category"]  = def->category;
    option["tooltip"]   = def->tooltip;
    option["sidetext"]  = def->sidetext;
    if (def->min > -FLT_MAX)
        option["min"] = def->min;
    else
        option["min"] = nullptr;
    if (def->max < FLT_MAX)
        option["max"] = def->max;
    else
        option["max"] = nullptr;
    option["mode"]       = mode_name(def->mode);
    option["enumValues"] = def->enum_values;
    option["enumLabels"] = def->enum_labels;
    option["readonly"]   = def->readonly;
    std::string why;
    option["writable"] = key_writable(key, why);
    if (cfg != nullptr && cfg->option(key) != nullptr)
        option["value"] = cfg->opt_serialize(key);
    else
        option["value"] = nullptr;
    return option;
}

json slice_summary_json(PartPlate* plate)
{
    if (plate == nullptr || !plate->is_slice_result_valid())
        return json();

    json summary = json::object();
    if (const GCodeProcessorResult* result = plate->get_slice_result(); result != nullptr) {
        const PrintEstimatedStatistics& stats = result->print_statistics;
        const auto& normal = stats.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
        summary["timeSeconds"]        = normal.time;
        summary["prepareTimeSeconds"] = normal.prepare_time;
        summary["filamentChanges"]    = stats.total_filament_changes;
        summary["travelDistanceMm"]   = stats.total_travel_distance;
        summary["toolpathOutside"]    = result->toolpath_outside;
        summary["gcodeCheckError"]    = result->gcode_check_result.error_code;
    }
    if (const Print* print = plate->fff_print(); print != nullptr) {
        const PrintStatistics& stats = print->print_statistics();
        summary["filamentMm"] = stats.total_used_filament;
        summary["filamentG"]  = stats.total_weight;
        summary["cost"]       = stats.total_cost;
    }
    return summary;
}

} // namespace WebAppSettings

// ---------------------------------------------------------------------------------------------
// WebAppBridgeWriteScope
// ---------------------------------------------------------------------------------------------

namespace {
int s_bridge_write_depth = 0; // GUI thread only
}

WebAppBridgeWriteScope::WebAppBridgeWriteScope() { ++s_bridge_write_depth; }
WebAppBridgeWriteScope::~WebAppBridgeWriteScope() { --s_bridge_write_depth; }
bool WebAppBridgeWriteScope::active() { return s_bridge_write_depth > 0; }

} // namespace GUI
} // namespace Slic3r
