#include "BeltTransform.hpp"
#include "Geometry.hpp"

namespace Slic3r {

Matrix3d BeltTransformPipeline::axis_rotation(const PrintConfig &config, double angle_deg)
{
    if (! config.belt_printer.value || std::abs(angle_deg) <= EPSILON)
        return Matrix3d::Identity();
    Vec3d unit_axis;
    switch (config.belt_slice_rotation.value) {
    case BeltRotationAxis::X: unit_axis = Vec3d::UnitX(); break;
    case BeltRotationAxis::Y: unit_axis = Vec3d::UnitY(); break;
    case BeltRotationAxis::Z: unit_axis = Vec3d::UnitZ(); break;
    default:                  return Matrix3d::Identity();
    }
    return Eigen::AngleAxisd(Geometry::deg2rad(angle_deg), unit_axis).toRotationMatrix();
}

Matrix3d BeltTransformPipeline::build_rotation_matrix(const PrintConfig &config)
{
    return axis_rotation(config, config.belt_slice_rotation_angle.value);
}

Matrix3d BeltTransformPipeline::build_gantry_matrix(const PrintConfig &config)
{
    return axis_rotation(config, config.belt_frame_tilt_angle.value);
}

} // namespace Slic3r
