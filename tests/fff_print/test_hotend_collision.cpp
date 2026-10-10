#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/HotendCollisionCheck.hpp"

#include <cmath>

using namespace Slic3r;

// Synthetic processed-move streams: no slicing, just geometry, so the envelope test itself is pinned.
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
// sweep the envelope through the wall, and that is a collision the check should (and does) report.
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

// A 10 mm wall standing off to one side of the origin, z = 0.2 .. 30: `along_x` runs it along X
// at y = `offset`, otherwise along Y at x = `offset`. Printed under tool axis `axis`.
void add_side_wall(GCodeProcessorResult &r, unsigned int &line, bool along_x, float offset, uint16_t axis)
{
    for (int k = 1; k <= 150; ++ k) {
        const float z = 0.2f * k;
        const Vec3f a = along_x ? Vec3f(-5.f, offset, z) : Vec3f(offset, -5.f, z);
        const Vec3f b = along_x ? Vec3f( 5.f, offset, z) : Vec3f(offset,  5.f, z);
        r.moves.push_back(move(EMoveType::Travel,  a, axis, line ++));
        r.moves.push_back(move(EMoveType::Extrude, b, axis, line ++, 0.4f));
    }
}

// Then, under tool axis `axis`, a drop from well above everything onto a short extrusion at the
// origin, 1 mm up. The hop off the wall happens under the wall's own axis, as in add_tilted_pass.
void add_pass_at_origin(GCodeProcessorResult &r, unsigned int &line, uint16_t wall_axis, uint16_t axis)
{
    r.moves.push_back(move(EMoveType::Travel,  Vec3f(0.f, 20.f, 75.f), wall_axis, line ++));
    r.moves.push_back(move(EMoveType::Travel,  Vec3f(0.f, 0.f, 75.f), axis, line ++));
    r.moves.push_back(move(EMoveType::Travel,  Vec3f(0.f, 0.f, 1.f), axis, line ++));
    r.moves.push_back(move(EMoveType::Extrude, Vec3f(0.3f, 0.3f, 1.f), axis, line ++, 0.1f));
}

HotendCollisionParams sides(double xpos, double xneg, double ypos, double yneg)
{
    HotendCollisionParams p;
    p.envelope.angle = { xpos, xneg, ypos, yneg };
    return p;
}

} // namespace

SCENARIO("Hotend pyramid collision check on processed moves", "[cutplanes][hotend]") {
    HotendCollisionParams params;   // four 45-degree sides (the old 90-degree cone), 40 mm tall

    GIVEN("a planar wall and a later chunk whose tool axis leans 45 degrees toward it") {
        GCodeProcessorResult r;
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 0.70710678f, 0.70710678f) };
        unsigned int line = 1;
        add_wall(r, line);
        // Tip 15 mm in front of the wall, 1 mm up. In the tilted frame the wall at z ~ 20 is ~24 mm up
        // the axis and ~3 mm off it -- deep inside the envelope.
        add_tilted_pass(r, line, Vec3f(5.f, -15.f, 1.f));

        THEN("the envelope hits the wall and the report names the chunks and G-code lines") {
            const auto res = check_hotend_collisions(r, params);
            REQUIRE(res.has_value());
            CHECK(res->_objName1.find("chunk 2") != std::string::npos);
            CHECK(res->_objName2.find("chunk 1") != std::string::npos);
            CHECK(res->message.find("Hotend collision") != std::string::npos);
            INFO(res->message);
            // The deepest hit is somewhere on the descent onto the tip (Z 60 -> 1), where the
            // envelope reaches furthest up the wall; which sample wins depends on the sampling, so
            // only the range is pinned.
            CHECK(res->_height >= 1.0);
            CHECK(res->_height <= 60.0);
        }
        THEN("shrinking the hotend model until it cannot reach the wall clears the report") {
            HotendCollisionParams small = params;
            small.envelope.height = 5.;     // the wall starts 15 mm away: a 5 mm envelope cannot reach it
            CHECK(! check_hotend_collisions(r, small).has_value());
        }
        THEN("a zero envelope height disables the check") {
            HotendCollisionParams off = params;
            off.envelope.height = 0.;
            CHECK(! check_hotend_collisions(r, off).has_value());
        }
    }

    GIVEN("the same wall with the later chunk far away") {
        GCodeProcessorResult r;
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 0.70710678f, 0.70710678f) };
        unsigned int line = 1;
        add_wall(r, line);
        add_tilted_pass(r, line, Vec3f(5.f, -200.f, 1.f));   // ~150 mm up the axis: beyond the 40 mm envelope
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

SCENARIO("Hotend pyramid: each side is tested at its own angle", "[cutplanes][hotend]") {
    // The wall stands 8 mm to one side of a vertical nozzle and rises 29 mm above its tip. A side
    // reaches 29 / tan(angle) + 0.5 mm out at that height, so 45 degrees (29.5 mm) reaches it and
    // 80 degrees (5.6 mm) does not. The wall was printed under a slightly tilted axis purely so the
    // later, vertical pass is a new episode.
    // Fills `r` in place: GCodeProcessorResult holds a mutex, so it can be neither copied nor moved.
    auto scenario = [](GCodeProcessorResult &r, float wall_x) {
        r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.0998f, 0.f, 0.995f) };
        unsigned int line = 1;
        add_side_wall(r, line, false, wall_x, 1);
        add_pass_at_origin(r, line, 1, 0);
    };

    GIVEN("a wall on the +X side") {
        GCodeProcessorResult r;
        scenario(r, 8.f);
        THEN("a slim +X side clears it, however fat the opposite side is") {
            CHECK(! check_hotend_collisions(r, sides(80., 20., 45., 45.)).has_value());
        }
        THEN("a fat +X side hits it, and the report names that side") {
            const auto res = check_hotend_collisions(r, sides(45., 80., 45., 45.));
            REQUIRE(res.has_value());
            INFO(res->message);
            CHECK(res->message.find("+X side") != std::string::npos);
        }
    }
    GIVEN("the mirror image: a wall on the -X side") {
        GCodeProcessorResult r;
        scenario(r, -8.f);
        THEN("a slim -X side clears it, however fat the opposite side is") {
            CHECK(! check_hotend_collisions(r, sides(20., 80., 45., 45.)).has_value());
        }
        THEN("a fat -X side hits it, and the report names that side") {
            const auto res = check_hotend_collisions(r, sides(80., 45., 45., 45.));
            REQUIRE(res.has_value());
            INFO(res->message);
            CHECK(res->message.find("-X side") != std::string::npos);
        }
    }
}

SCENARIO("Hotend pyramid: the sides turn with the head", "[cutplanes][hotend]") {
    // A wall 12 mm toward bed +Y, and a pass whose tool axis leans 5 degrees toward +Y. A 5-axis
    // machine reaches that axis with the table at C90, so the head's +X side -- the side B leans
    // it toward -- faces bed +Y and is the side that meets the wall. A frame that merely tipped +Z
    // over (the minimal rotation) would have put the head's +Y side there instead.
    GCodeProcessorResult r;
    r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 0.0872f, 0.9962f) };
    unsigned int line = 1;
    add_side_wall(r, line, true, 12.f, 0);
    add_pass_at_origin(r, line, 0, 1);

    GIVEN("a real 5-axis machine") {
        THEN("a slim head +X side clears the bed +Y wall even with a fat head +Y side") {
            CHECK(! check_hotend_collisions(r, sides(80., 20., 20., 20.)).has_value());
        }
        THEN("a fat head +X side hits it") {
            const auto res = check_hotend_collisions(r, sides(45., 80., 80., 80.));
            REQUIRE(res.has_value());
            INFO(res->message);
            CHECK(res->message.find("+X side") != std::string::npos);
        }
    }
    GIVEN("a virtual 5-axis printer, where the nozzle never tilts") {
        THEN("the envelope stands upright with its sides along machine X / Y, whatever the tagged axis") {
            HotendCollisionParams fat_x = sides(45., 80., 80., 80.);
            fat_x.upright = true;
            CHECK(! check_hotend_collisions(r, fat_x).has_value());   // the wall is on the slim +Y side
            HotendCollisionParams fat_y = sides(80., 80., 45., 80.);
            fat_y.upright = true;
            const auto res = check_hotend_collisions(r, fat_y);
            REQUIRE(res.has_value());
            INFO(res->message);
            CHECK(res->message.find("+Y side") != std::string::npos);
        }
    }
}

SCENARIO("Hotend envelope: a sphere about the tip caps the pyramid", "[cutplanes][hotend]") {
    // A low-clearance head: a 10-degree +X side spreads 5.7 mm per mm of height, so the bare pyramid
    // reached a wall 45 mm away once ~8 mm up. The envelope is that pyramid clipped by a sphere of
    // radius `height` about the tip -- a spherical cap instead of a flat top -- so a 40 mm envelope
    // never reaches 45 mm, however shallow the side, and a 60 mm one does.
    GCodeProcessorResult r;
    r.tool_axes = { Vec3f(0.f, 0.f, 1.f), Vec3f(0.0998f, 0.f, 0.995f) };
    unsigned int line = 1;
    add_side_wall(r, line, false, 45.f, 1);
    add_pass_at_origin(r, line, 1, 0);

    THEN("a 40 mm radius stops short of the wall") {
        CHECK(! check_hotend_collisions(r, sides(10., 80., 80., 80.)).has_value());
    }
    THEN("a 60 mm radius reaches it, on the +X side") {
        HotendCollisionParams big = sides(10., 80., 80., 80.);
        big.envelope.height = 60.;
        const auto res = check_hotend_collisions(r, big);
        REQUIRE(res.has_value());
        INFO(res->message);
        CHECK(res->message.find("+X side") != std::string::npos);
    }
}
