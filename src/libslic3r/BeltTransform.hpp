#pragma once

#include "libslic3r.h"
#include "Point.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// Belt-printer tilt math shared by the slicing frame, the machine frame and the GUI
// (BELT_MOD, kept from OrcaSlicer's BeltTransformPipeline).
//
// A belt printer is this fork's single-segment multi-planar case. Two angles about the printer's
// tilt axis (`belt_slice_rotation`) matter, and they are deliberately separate settings:
//
//   - the SLICING angle, `belt_slice_rotation_angle`, a process option: the angle between the
//     layers and the belt. Un-cut objects are sliced rotated by build_rotation_matrix()
//     (PrintObject::update_slice_frame) and the per-move segment trafo undoes it at emission;
//   - the machine's TILT, `belt_frame_tilt_angle`, a printer option: the physical angle between
//     the gantry and the belt. It drives MachineFrameTransform (the shear/scale into the machine
//     axes), the rotary home and the preview's nozzle, through build_gantry_matrix().
//
// They normally agree (45 on most machines); Print::validate() refuses a print whose slicing
// angle strays further from the tilt than `belt_slice_angle_tolerance`. The angle convention is
// OrcaSlicer's, so belt printer profiles carry over unchanged.
class BeltTransformPipeline
{
public:
    static bool has_rotation(const PrintConfig &config)
    {
        return config.belt_printer.value
            && config.belt_slice_rotation.value != BeltRotationAxis::None
            && std::abs(config.belt_slice_rotation_angle.value) > EPSILON;
    }

    // The 3x3 slicing rotation (belt_slice_rotation_angle about belt_slice_rotation) -- Identity
    // when the axis is None, the angle is ~0 or belt mode is off.
    static Matrix3d build_rotation_matrix(const PrintConfig &config);
    // The 3x3 rotation of the machine's gantry (belt_frame_tilt_angle about the same axis).
    static Matrix3d build_gantry_matrix(const PrintConfig &config);

private:
    static Matrix3d axis_rotation(const PrintConfig &config, double angle_deg);
};

} // namespace Slic3r
