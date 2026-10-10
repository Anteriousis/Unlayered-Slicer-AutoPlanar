#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp"

#include <cmath>

using namespace Slic3r;

// UNLAYERED_MOD -- the virtual-5-axis tilt limit, pinned as pure geometry: no printer, no slicing,
// no GUI. Both enforcement points (the cut-plane gizmo's drag clamp and Print::validate()) call
// this one function, so if these numbers are right the two cannot disagree about what is safe.
//
// See virtual_tilt_limit_deg() and HotendEnvelope in PrintConfig.hpp for the derivation.

namespace {
// Generous enough that the gantry never binds, so the hotend term can be measured on its own.
constexpr double HUGE_GANTRY = 1.0e6;

// Which way a build normal leans. A chunk leaning toward +X climbs toward -X, so its -X side binds.
constexpr double LEAN_POS_X = 0.;
constexpr double LEAN_NEG_X = PI;
constexpr double LEAN_POS_Y = 0.5 * PI;
constexpr double LEAN_NEG_Y = -0.5 * PI;

HotendEnvelope envelope(double xpos, double xneg, double ypos, double yneg, double height = 40.)
{
    HotendEnvelope e;
    e.height = height;
    e.angle  = { xpos, xneg, ypos, yneg };
    return e;
}
HotendEnvelope square(double angle) { return envelope(angle, angle, angle, angle); }
// Every side flush with the nozzle: the hotend itself never limits anything.
HotendEnvelope needle() { return square(90.); }

} // namespace

SCENARIO("virtual 5-axis tilt limit: the hotend pyramid", "[VirtualTiltLimit]")
{
    GIVEN("a printer whose gantry is effectively out of reach") {
        THEN("tilting straight at one side, the limit is that side's angle from the bed") {
            // The old cone test's guard, restated: the angle that limits tilt is the COMPLEMENT of
            // the face's angle from the nozzle axis, which is its angle from the bed. At 45 the two
            // coincide, so only the unequal sides below tell a right implementation from a wrong one.
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(30.)); // climbs toward -X
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_NEG_X) == Catch::Approx(80.)); // climbs toward +X
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_Y) == Catch::Approx(45.)); // climbs toward -Y
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_NEG_Y) == Catch::Approx(60.)); // climbs toward +Y
        }

        THEN("the side that binds is the one the layer climbs toward") {
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            CHECK(virtual_tilt_binding_face(e, LEAN_POS_X) == HotendEnvelope::XNeg);
            CHECK(virtual_tilt_binding_face(e, LEAN_NEG_X) == HotendEnvelope::XPos);
            CHECK(virtual_tilt_binding_face(e, LEAN_POS_Y) == HotendEnvelope::YNeg);
            CHECK(virtual_tilt_binding_face(e, LEAN_NEG_Y) == HotendEnvelope::YPos);
        }

        THEN("a slimmer side allows MORE tilt toward it") {
            CHECK(virtual_tilt_limit_deg(envelope(45., 70., 45., 45.), HUGE_GANTRY, 0., LEAN_POS_X) >
                  virtual_tilt_limit_deg(envelope(45., 20., 45., 45.), HUGE_GANTRY, 0., LEAN_POS_X));
        }

        THEN("between two sides the pyramid's corner leads, so a diagonal is stricter than either side") {
            // Four 45-degree sides are the old 90-degree cone along each axis, but at 45 degrees of
            // azimuth the corner stands sqrt(2) further out: atan(1/sqrt(2)) = 35.26.
            const HotendEnvelope e = square(45.);
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., 0.25 * PI) == Catch::Approx(35.264).margin(0.01));
        }

        THEN("an unknown direction yields the worst one, which no direction undercuts") {
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            const double worst = virtual_tilt_limit_deg(e, HUGE_GANTRY, 0.);
            double sampled_min = 90.;
            for (int i = 0; i < 3600; ++ i) {
                const double limit = virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., 2. * PI * i / 3600.);
                CHECK(limit >= worst - 1e-9);
                sampled_min = std::min(sampled_min, limit);
            }
            CHECK(sampled_min == Catch::Approx(worst).margin(0.01));
        }

        THEN("a zero envelope height switches the hotend term off") {
            CHECK(virtual_tilt_hotend_term_deg(envelope(10., 10., 10., 10., 0.), LEAN_POS_X) == Catch::Approx(90.));
            CHECK(virtual_tilt_limit_deg(envelope(10., 10., 10., 10., 0.), HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(90.));
        }

        THEN("out-of-range side angles are clamped rather than going nonsense") {
            for (const double angle : { -5., 0., 90., 200. }) {
                const double limit = virtual_tilt_limit_deg(square(angle), HUGE_GANTRY, 0., LEAN_POS_X);
                CHECK(limit > 0.);
                CHECK(limit <= 90.);
            }
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: the gantry", "[VirtualTiltLimit]")
{
    GIVEN("40 mm of headroom under the gantry and a hotend that never binds") {
        THEN("the limit is the angle whose climb across the chunk just fits") {
            // A chunk 80 mm across climbing at atan(40/80) rises exactly 40 mm end to end.
            CHECK(virtual_tilt_limit_deg(needle(), 40., 80., LEAN_POS_X) == Catch::Approx(26.565).margin(0.01));
            // Square case: as wide as it is tall -> 45 degrees.
            CHECK(virtual_tilt_limit_deg(needle(), 40., 40., LEAN_POS_X) == Catch::Approx(45.));
        }

        THEN("a wider chunk is allowed less tilt") {
            const double narrow = virtual_tilt_limit_deg(square(45.), 40., 40., LEAN_POS_X);
            const double wide   = virtual_tilt_limit_deg(square(45.), 40., 200., LEAN_POS_X);
            CHECK(wide < narrow);
        }

        THEN("an unknown reach falls back to the hotend term alone") {
            // 0 means "not derived yet" -- the looser answer, so a caller that cannot supply a
            // reach never over-restricts the user. Print::validate() always can.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 0., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(45.), 40., -1., LEAN_POS_X) == Catch::Approx(45.));
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: whichever constraint is stricter wins", "[VirtualTiltLimit]")
{
    GIVEN("four 45 degree sides and 40 mm of gantry clearance") {
        THEN("a small chunk is bounded by the hotend and a large one by the gantry") {
            // 20 mm across: the gantry would allow atan(40/20) = 63 degrees, so the hotend's 45 binds.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 20., LEAN_POS_X) == Catch::Approx(45.));
            // 160 mm across: the gantry allows atan(40/160) = 14, well inside the hotend's 45.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 160., LEAN_POS_X) == Catch::Approx(14.036).margin(0.01));
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: each term has its own off switch", "[VirtualTiltLimit]")
{
    GIVEN("no gantry height") {
        THEN("the hotend still limits the tilt, whatever the chunk's size") {
            // 0 only means the gantry was never measured. The upright nozzle meets its own hotend
            // regardless, and that limit used to be skipped too, so a steep chunk printed straight
            // into it on a 3-axis machine.
            CHECK(virtual_tilt_limit_deg(square(45.), 0., 50., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(45.), 0., 5000., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(75.), 0., 50.) == Catch::Approx(virtual_tilt_hotend_term_deg(square(75.), std::nullopt)));
            CHECK(virtual_tilt_limit_deg(square(45.), -1., 50., LEAN_POS_X) == Catch::Approx(45.));
        }
        THEN("with the hotend envelope off too, there is no limit at all") {
            // 0 is the agreed "no limit" signal to every caller.
            CHECK(virtual_tilt_limit_deg(envelope(45., 45., 45., 45., 0.), 0., 50., LEAN_POS_X) == 0.);
            CHECK(virtual_tilt_limit_deg(envelope(45., 45., 45., 45., 0.), -1., 50.) == 0.);
        }
    }

    GIVEN("a geometry so hostile that the honest answer is ~0 degrees") {
        THEN("it still reports a limit rather than reading as 'no limit'") {
            // A 2 m chunk under 1 mm of clearance. The real answer rounds to zero, but returning 0
            // would mean "unlimited" to every caller and open the machine right up, so the floor
            // has to stay above it.
            const double limit = virtual_tilt_limit_deg(square(45.), 1., 2000., LEAN_POS_X);
            CHECK(limit > 0.);
            CHECK(limit < 1.);
        }
    }
}

SCENARIO("tool_head_frame: the head's real orientation for a tool axis", "[VirtualTiltLimit][hotend]")
{
    auto check_rotation = [](const Matrix3d &R) {
        CHECK((R * R.transpose()).isApprox(Matrix3d::Identity(), 1e-9));
        CHECK(R.determinant() == Catch::Approx(1.));
    };
    GIVEN("a vertical tool axis") {
        THEN("the head frame is the bed frame (C = 0, the home pose)") {
            CHECK(tool_head_frame(Vec3d::UnitZ()).isApprox(Matrix3d::Identity()));
        }
    }
    GIVEN("a tool axis leaning 30 degrees toward +X (B-60 C0)") {
        const Matrix3d R = tool_head_frame(Vec3d(0.5, 0., std::sqrt(0.75)));
        THEN("the head's +X side tips with it and +Y, the B axis, stays level") {
            check_rotation(R);
            CHECK(Vec3d(R.row(0).transpose()).isApprox(Vec3d(std::sqrt(0.75), 0., -0.5), 1e-9));
            CHECK(Vec3d(R.row(1).transpose()).isApprox(Vec3d(0., 1., 0.), 1e-9));
            CHECK(Vec3d(R.row(2).transpose()).isApprox(Vec3d(0.5, 0., std::sqrt(0.75)), 1e-9));
        }
    }
    GIVEN("a tool axis leaning 30 degrees toward +Y (the table turned to C90)") {
        const Matrix3d R = tool_head_frame(Vec3d(0., 0.5, std::sqrt(0.75)));
        THEN("the table has turned under the head, so the head's +X side faces bed +Y") {
            check_rotation(R);
            CHECK(Vec3d(R.row(0).transpose()).isApprox(Vec3d(0., std::sqrt(0.75), -0.5), 1e-9));
            CHECK(Vec3d(R.row(1).transpose()).isApprox(Vec3d(-1., 0., 0.), 1e-9));
        }
    }
}

SCENARIO("hotend envelope: the single cone angle migrates to four sides", "[VirtualTiltLimit][hotend]")
{
    auto migrated = [](double legacy_full_angle) {
        DynamicPrintConfig cfg;
        cfg.apply(FullPrintConfig::defaults());
        cfg.set_key_value("nonplanar_hotend_cone_angle", new ConfigOptionFloat(legacy_full_angle));
        migrate_legacy_hotend_cone(cfg);
        CHECK(! cfg.has("nonplanar_hotend_cone_angle"));
        return HotendEnvelope::from_config(cfg);
    };
    THEN("the default 90 degree cone becomes four 45 degree sides") {
        const HotendEnvelope e = migrated(90.);
        for (double a : e.angle) CHECK(a == Catch::Approx(45.));
        CHECK(e.height == Catch::Approx(40.));
    }
    THEN("a 60 degree cone becomes four 60 degree sides") {
        for (double a : migrated(60.).angle) CHECK(a == Catch::Approx(60.));
    }
    THEN("0, which used to disable the check, becomes a zero envelope height") {
        CHECK(! migrated(0.).enabled());
    }
    THEN("the defaults never carry the legacy key, so nothing is migrated that was not loaded") {
        DynamicPrintConfig cfg;
        cfg.apply(FullPrintConfig::defaults());
        CHECK(! cfg.has("nonplanar_hotend_cone_angle"));
        cfg.set_key_value("nonplanar_hotend_angle_xneg", new ConfigOptionFloat(20.));
        migrate_legacy_hotend_cone(cfg);
        CHECK(HotendEnvelope::from_config(cfg).angle[HotendEnvelope::XNeg] == Catch::Approx(20.));
    }
}

TEST_CASE("A virtual 5-axis slice steeper than the hotend allows is refused without a gantry height", "[VirtualTiltLimit]")
{
    // Every side of the hotend at 5 degrees from the bed, so tilting along X the hotend allows 5
    // degrees. The gantry height is left at 0, i.e. not measured, which used to switch the whole
    // limit off and let a steep chunk print straight into the hotend.
    const double allowed = 5.;
    auto validate = [allowed](double tilt) {
        Model        model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",             "1" },
            { "axis_selection",               "virtual" },
            { "nonplanar_gantry_height",      0 },
            { "nonplanar_virtual_max_angle",  0 },
            { "nonplanar_hotend_cone_height", 40 },
            { "nonplanar_hotend_angle_xpos",  allowed },
            { "nonplanar_hotend_angle_xneg",  allowed },
            { "nonplanar_hotend_angle_ypos",  allowed },
            { "nonplanar_hotend_angle_yneg",  allowed },
            { "layer_rotation_b",             tilt },
            { "layer_rotation_c",             0 },
            { "layer_change_gcode",           "G92 E0" },
        });
        Print print;
        print.apply(model, config);
        REQUIRE(print.config().virtual_5axis.value);
        return print.validate();
    };

    const StringObjectException refused = validate(2. * allowed);
    INFO(refused.string);
    CHECK(refused.string.find("allows at most 5.0 degrees") != std::string::npos);
    // The hotend, not the gantry, is named as what binds.
    CHECK(refused.opt_key.rfind("nonplanar_hotend_angle_", 0) == 0);

    const StringObjectException accepted = validate(allowed - 1.);
    INFO(accepted.string);
    CHECK(accepted.string.empty());
}
