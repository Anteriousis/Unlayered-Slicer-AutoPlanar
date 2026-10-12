#pragma once

#include "PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r::CurvedTransition {

// One FIXED active physical nozzle on a shared moving carriage. Its measured
// envelope must include every parked nozzle, sock, duct and other moving part,
// all expressed about this active tip. Independent/docked heads need a separate
// stationary-obstacle and switching-path planner; this is not that planner.
struct ToolheadState {
    size_t nozzle = 0;
    HotendEnvelope envelope;
    double gantry_height = 0., max_z = 0., min_layer = 0., max_layer = 0., z_speed = 0.;

    static ToolheadState from_config(const PrintConfig& config, size_t active, bool dynamic_mapping = false)
    {
        const size_t count = config.nozzle_diameter.size();
        if (count == 0 || active >= count)
            throw std::invalid_argument("Curved blend has no valid fixed active nozzle mapping.");
        ToolheadState state;
        state.nozzle = active;
        state.envelope = HotendEnvelope::from_config(config);
        state.gantry_height = config.nonplanar_gantry_height.value;
        state.max_z = config.nonplanar_virtual_max_z.value;
        if (count > 1 || config.nonplanar_shared_carriage.value) {
            if (!config.nonplanar_shared_carriage.value)
                throw std::invalid_argument("Curved multi-nozzle printing currently requires a shared moving carriage; independent and docked heads need additional obstacle planning.");
            if (config.nonplanar_measured_nozzle_count.value != int(count))
                throw std::invalid_argument("The measured carriage nozzle count does not match this printer. Adding or removing a nozzle requires reviewing every whole-carriage envelope.");
            if (count>1 && (dynamic_mapping || config.filament_map_mode.value < fmmManual))
                throw std::invalid_argument("Curved multi-nozzle printing requires a fixed manual filament-to-nozzle mapping; automatic nozzle reassignment is not supported.");
            auto exact = [count](const auto& option) { return option.values.size() == count; };
            if (!exact(config.nonplanar_state_nozzle_id) || !exact(config.nonplanar_state_complete) ||
                !exact(config.nonplanar_state_envelope_height) || !exact(config.nonplanar_state_angle_xpos) ||
                !exact(config.nonplanar_state_angle_xneg) || !exact(config.nonplanar_state_angle_ypos) ||
                !exact(config.nonplanar_state_angle_yneg) || !exact(config.nonplanar_state_gantry_height) ||
                !exact(config.nonplanar_state_max_z) || !exact(config.nonplanar_state_source) ||
                config.nonplanar_state_nozzle_id.values[active] != int(active + 1) ||
                !config.nonplanar_state_complete.values[active] ||
                config.nonplanar_state_source.values[active].empty())
                throw std::invalid_argument("The active nozzle needs an explicitly identified complete toolhead state, including parked nozzles, with its own recorded clearance source. Generic or copied defaults are insufficient.");
            state.envelope.height = config.nonplanar_state_envelope_height.values[active];
            state.envelope.angle = {{config.nonplanar_state_angle_xpos.values[active],
                config.nonplanar_state_angle_xneg.values[active], config.nonplanar_state_angle_ypos.values[active],
                config.nonplanar_state_angle_yneg.values[active]}};
            state.gantry_height = config.nonplanar_state_gantry_height.values[active];
            const double bound = config.nonplanar_state_max_z.values[active];
            if (!std::isfinite(bound) || bound<=0. || !std::isfinite(state.max_z))
                throw std::invalid_argument("Curved blend needs a finite positive Z bound for the active state.");
            state.max_z = state.max_z > 0. ? std::min(state.max_z, bound) : bound;
            if (config.extruder_offset.values.size() != count || !config.extruder_offset.values[active].allFinite() ||
                config.min_layer_height.values.size() != count || config.max_layer_height.values.size() != count ||
                config.extruder_printable_area.values.size() != count ||
                config.extruder_printable_area.values[active].size() < 3 ||
                std::any_of(config.extruder_printable_area.values[active].begin(), config.extruder_printable_area.values[active].end(),
                    [](const Vec2d& p) { return !p.allFinite(); }) ||
                config.extruder_printable_height.values.size() != count ||
                !std::isfinite(config.extruder_printable_height.values[active]) ||
                config.extruder_printable_height.values[active] <= 0.)
                throw std::invalid_argument("Curved multi-nozzle printing requires explicit active-nozzle offsets, printable areas/heights and layer limits.");
            state.max_z = std::min(state.max_z, config.extruder_printable_height.values[active]);
        }
        for (double angle : state.envelope.angle)
            if (!std::isfinite(angle) || angle <= 0. || angle >= 90.)
                throw std::invalid_argument("Curved blend needs finite conservative side angles between zero and 90 degrees for the whole active toolhead state.");
        if (!std::isfinite(state.envelope.height) || state.envelope.height <= 0. ||
            !std::isfinite(state.gantry_height) || state.gantry_height <= 0. ||
            !std::isfinite(state.max_z) || state.max_z <= 0. ||
            !std::isfinite(config.printable_height.value) || config.printable_height.value <= 0.)
            throw std::invalid_argument("Curved blend needs positive configured toolhead, gantry and machine Z bounds for the active nozzle.");
        state.max_z = std::min(state.max_z, config.printable_height.value);
        state.min_layer = config.min_layer_height.get_at(active);
        state.max_layer = config.max_layer_height.get_at(active);
        if (!std::isfinite(state.min_layer) || !std::isfinite(state.max_layer) ||
            state.min_layer <= 0. || state.max_layer < state.min_layer)
            throw std::invalid_argument("Curved blend has invalid active-nozzle layer height limits.");
        state.z_speed = INFINITY;
        // Match the writer's current normal-mode stride; inactive silent-mode
        // placeholders may be zero. Use the slowest normal limit conservatively.
        const size_t stride = printer_options_with_variant_2.count("machine_max_speed_z") ? 2 : 1;
        for (size_t i=0; i<config.machine_max_speed_z.values.size(); i+=stride) {
            const double speed=config.machine_max_speed_z.values[i];
            if (!std::isfinite(speed) || speed <= 0.)
                throw std::invalid_argument("Curved blend requires positive finite machine Z speed limits.");
            state.z_speed = std::min(state.z_speed, speed);
        }
        if (!std::isfinite(state.z_speed))
            throw std::invalid_argument("Curved blend has no configured machine Z speed.");
        return state;
    }
};
} // namespace Slic3r::CurvedTransition
