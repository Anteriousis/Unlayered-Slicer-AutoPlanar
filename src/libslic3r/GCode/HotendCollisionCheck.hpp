#ifndef slic3r_HotendCollisionCheck_hpp_
#define slic3r_HotendCollisionCheck_hpp_

#include "GCodeProcessor.hpp"

#include <optional>

namespace Slic3r {

// UNLAYERED_MOD -- post-export hotend collision check for multiplanar prints.
//
// The hotend is modelled as a cone with its apex at the nozzle tip and its axis along the tool
// axis: `cone_angle_deg` is the FULL opening angle (90 = a 45-degree half-angle), `cone_height`
// how far up the axis the model reaches. Material deposited in an EARLIER "episode" -- a maximal
// run of moves sharing one tool axis, i.e. one chunk or cut-plane segment under By object / By
// group sequencing -- that ends up inside that cone while a later episode prints is a collision.
//
// Frame: RepRapFirmware compensates the rotary axes, so the processed move positions are the
// nozzle tip in the BED frame and deposited material stays where it was laid; the tool axis per
// move is GCodeProcessorResult::tool_axes[move.tool_axis_id], which the exporter tags at every
// orientation change. A planar print has a single tool axis and is never checked (returns
// nullopt immediately), so ordinary slicing pays nothing.
struct HotendCollisionParams
{
    double cone_height    = 40.;   // mm along the tool axis
    double cone_angle_deg = 90.;   // full opening angle; <= 0 disables the check
    double margin         = 0.5;   // lateral tolerance (line width / 2 + nozzle flat), mm
    double eps            = 0.3;   // ignore material within this height above the tip: the layer being printed
};

// Returns a ConflictResult describing the worst hit (with `message` set), or nullopt when the
// cone stays clear of all earlier material. _objName1/_objName2 name the printing chunk and the
// hit material's chunk, _height is the machine Z of the offending tip position.
std::optional<ConflictResult> check_hotend_collisions(const GCodeProcessorResult &result, const HotendCollisionParams &params);

} // namespace Slic3r

#endif // slic3r_HotendCollisionCheck_hpp_
