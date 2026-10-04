#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/HotendCollisionCheck.hpp"

#include <cmath>

using namespace Slic3r;

// Synthetic processed-move streams: no slicing, just geometry, so the cone test itself is pinned.
namespace {

GCodeProcessorResult::MoveVertex move(EMoveType type, const Vec3f &pos, uint16_t axis, unsigned int line, float de = 0.f)
{
    GCodeProcessorResult::MoveVertex m;
    m.type           = type;
    m.position       = pos;
    m.tool_axis_id   = axis;
    m.gcode_id       = line;
    m.delta_extruder = de;
    m.width = 0.4f; m.height = 0.2f;
    return m;
}

// Episode 0: a 10 mm long wall along X at y = 0, built from z = 0.2 to 30 in 0.2 mm layers (planar, axis 0).
void add_wall(GCodeProcessorResult &r, unsigned int &line)
{
    for (int k = 1; k <= 150; ++ k) {
        const float z = 0.2f * k;
        r.moves.push_back(move(EMoveType::Travel,  Vec3f(0.f, 0.f, z), 0, line ++));
        r.moves.push_back(move(EMoveType::Extrude, Vec3f(10.f, 0.f, z), 0, line ++, 0.4f));
    }
}

// Episode 1: a short extrusion at `tip` under tool axis 1. Like a real chunk transition, the head
// first travels to a staging point high above the target and then descends straight down onto
// it -- a travel that left the wall's top edge diagonally under the tilted axis would itself
// sweep the cone through the wall, and that is a collision the check should (and does) report.
void add_tilted_pass(GCodeProcessorResult &r, unsigned int &line, const Vec3f &tip)
{
    // The hop off the finished wall happens under the OLD axis, as in the exporter's transition
    // (the `; TOOL_AXIS` tag follows the hop and the rotation moves). Only then does the head
    // travel across and descend under the new axis.
    r.moves.push_back(move(EMoveType::Travel,  Vec3f(10.f, 0.f, 60.f), 0, line ++));
    r.moves.push_back(move(EMoveType::Travel,  Vec3f(tip.x(), tip.y(), 60.f), 1, line ++));
    r.moves.push_back(move(EMoveType::Travel,  tip, 1, line ++));
    r.moves.push_back(move(EMoveType::Extrude, tip + Vec3f(2.f, 0.f, 0.f), 1, line ++, 0.1f));
}

} // namespace

SCENARIO("Hotend cone collision check on processed moves", "[cutplanes][hotend]") {
    HotendCollisionParams params;   // 90-degree cone, 40 mm tall

    GIVEN("a planar wall and a later chunk whose tool axis leans 45 degrees toward it") {
        GCodeProcessorResult r;
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 0.70710678f, 0.70710678f) };
        unsigned int line = 1;
        add_wall(r, line);
        // Tip 15 mm in front of the wall, 1 mm up. In the tilted frame the wall at z ~ 20 is ~24 mm up
        // the axis and ~3 mm off it -- deep inside a 45-degree half-angle cone.
        add_tilted_pass(r, line, Vec3f(5.f, -15.f, 1.f));

        THEN("the cone hits the wall and the report names the chunks and G-code lines") {
            const auto res = check_hotend_collisions(r, params);
            REQUIRE(res.has_value());
            CHECK(res->_objName1.find("chunk 2") != std::string::npos);
            CHECK(res->_objName2.find("chunk 1") != std::string::npos);
            CHECK(res->message.find("Hotend collision") != std::string::npos);
            INFO(res->message);
            // The deepest hit is somewhere on the descent onto the tip (Z 60 -> 1), where the cone
            // reaches furthest up the wall; which sample wins depends on the sampling, so only the
            // range is pinned.
            CHECK(res->_height >= 1.0);
            CHECK(res->_height <= 60.0);
        }
        THEN("shrinking the hotend model until it cannot reach the wall clears the report") {
            HotendCollisionParams small = params;
            small.cone_height = 5.;     // the wall starts 15 mm away: a 5 mm cone cannot reach it
            CHECK(! check_hotend_collisions(r, small).has_value());
        }
        THEN("a zero cone angle disables the check") {
            HotendCollisionParams off = params;
            off.cone_angle_deg = 0.;
            CHECK(! check_hotend_collisions(r, off).has_value());
        }
    }

    GIVEN("the same wall with the later chunk far away") {
        GCodeProcessorResult r;
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 0.70710678f, 0.70710678f) };
        unsigned int line = 1;
        add_wall(r, line);
        add_tilted_pass(r, line, Vec3f(5.f, -200.f, 1.f));   // ~150 mm up the axis: beyond the 40 mm cone
        THEN("nothing is reported") {
            CHECK(! check_hotend_collisions(r, params).has_value());
        }
    }

    GIVEN("a print with a single tool axis") {
        GCodeProcessorResult r;
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f) };
        unsigned int line = 1;
        add_wall(r, line);
        r.moves.push_back(move(EMoveType::Travel,  Vec3f(5.f, -15.f, 1.f), 0, line ++));
        r.moves.push_back(move(EMoveType::Extrude, Vec3f(7.f, -15.f, 1.f), 0, line ++, 0.1f));
        THEN("the check is skipped: a planar print has nothing above the nozzle") {
            CHECK(! check_hotend_collisions(r, params).has_value());
        }
    }
}
