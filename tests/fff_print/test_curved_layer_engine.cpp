#include <catch2/catch_all.hpp>
#include "test_helpers.hpp"
#include "auto_planes_fixtures.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <sstream>
#include <regex>

using namespace Slic3r;
namespace {
DynamicPrintConfig curved_config()
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({
        {"curved_layer_blend","1"},{"enable_nonplanar","1"},{"axis_selection","virtual"},
        {"print_sequence","by group"},{"nonplanar_hotend_cone_height","125"},
        {"nonplanar_hotend_angle_xpos","7"},{"nonplanar_hotend_angle_xneg","10"},
        {"nonplanar_hotend_angle_ypos","15"},{"nonplanar_hotend_angle_yneg","6"},
        {"nonplanar_gantry_height","14"},{"nonplanar_virtual_max_angle","5"},{"nonplanar_virtual_max_z","100"},
        {"min_layer_height","0.08"},{"max_layer_height","0.32"},
        {"layer_height","0.2"},{"initial_layer_print_height","0.2"},
        {"sparse_infill_density","100%"},{"wall_generator","classic"},{"seam_slope_type","none"},
        {"enable_support","0"},{"raft_layers","0"},{"enable_prime_tower","0"},{"enable_arc_fitting","0"},
        {"wipe","0"},{"infill_combination","0"},{"skirt_loops","0"},
        {"layer_change_gcode","G92 E0"},{"before_layer_change_gcode",""},
        {"machine_start_gcode","G28"},{"machine_end_gcode","M104 S0"},{"thumbnails",""},
        {"use_relative_e_distances","1"},{"gcode_comments","1"},
        {"filament_adaptive_volumetric_speed","0"}
    });
    return c;
}
TriangleMesh ramp(double angle=5.)
{
    return TriangleMesh(AutoPlanesFixtures::heightfield_block(20.,20.,4,[angle](double x){return 3.+x*std::tan(angle*PI/180.);}));
}
}

TEST_CASE("Curved blend slices the flattened solid and writes physical paths", "[curved-engine]")
{
    Print print; Model model;
    Test::init_print({ramp()},print,model,curved_config());
    REQUIRE(print.validate().string.empty());
    const auto original_vertices=model.objects.front()->volumes.front()->mesh().its.vertices;
    const std::string gcode=Test::gcode(print);
    const auto* object=print.objects().front();
    REQUIRE(object->curved_field());
    REQUIRE(object->curved_ramp());
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original_vertices);
    REQUIRE(std::abs(object->layers().back()->print_z-object->curved_field()->reference_top_mm())<1e-8);
    REQUIRE(gcode.find("curved blend: traverse above solid")!=std::string::npos);
    REQUIRE(gcode.find("G2 ")==std::string::npos);
    REQUIRE(gcode.find("G3 ")==std::string::npos);
    REQUIRE(gcode.find(" B") ==std::string::npos);
    REQUIRE(gcode.find(" C") ==std::string::npos);
    // Verify every native deposition has XYZ, stays above the foundation and
    // below the true roof, including paths running across X and off-axis infill.
    std::istringstream in(gcode); std::string line;
    size_t extrusions=0; double minz=1e9,maxz=0.;
    const std::regex zword(R"( Z(-?\d*\.?\d+))");
    while(std::getline(in,line)) {
        const auto code=line.substr(0,line.find(';'));
        if (code.rfind("G1",0)!=0 || code.find(" E")==std::string::npos || code.find(" X")==std::string::npos) continue;
        REQUIRE(code.find(" Y")!=std::string::npos);
        std::smatch z; REQUIRE(std::regex_search(code,z,zword));
        minz=std::min(minz,std::stod(z[1])); maxz=std::max(maxz,std::stod(z[1])); ++extrusions;
    }
    REQUIRE(extrusions>1000);
    REQUIRE(minz>=.199);
    REQUIRE(maxz<=object->curved_ramp()->top_max+.001);
    REQUIRE(maxz>object->curved_field()->reference_top_mm()+.1);
}

TEST_CASE("Curved blend rejects unsupported inputs and excessive tilt", "[curved-engine]")
{
    SECTION("supports") {
        auto config=curved_config(); config.set_deserialize_strict({{"enable_support","1"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("several parts") {
        Print p; Model m; Test::init_print({ramp(),ramp()},p,m,curved_config());
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("curved roof not yet implemented") {
        Print p; Model m;
        Test::init_print({TriangleMesh(AutoPlanesFixtures::curved_top_block())},p,m,curved_config());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("roof beyond configured limit") {
        Print p; Model m; Test::init_print({ramp(6.)},p,m,curved_config());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("infeasible layer spacing") {
        auto config=curved_config(); config.set_deserialize_strict({{"min_layer_height","0.18"},{"max_layer_height","0.21"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_THROWS(Test::gcode(p));
    }
}

TEST_CASE("Curved blend config round trip and disabling restores planar slices", "[curved-engine]")
{
    auto config=curved_config();
    REQUIRE(config.opt_bool("curved_layer_blend"));
    DynamicPrintConfig restored;
    restored.set_deserialize_strict("curved_layer_blend",config.option("curved_layer_blend")->serialize());
    REQUIRE(restored.opt_bool("curved_layer_blend"));
    Print p; Model m; Test::init_print({ramp()},p,m,config);
    const auto curved=Test::gcode(p);
    config.set_deserialize_strict({{"curved_layer_blend","0"}});
    p.apply(m,config);
    const auto planar=Test::gcode(p);
    REQUIRE_FALSE(p.objects().front()->curved_field());
    REQUIRE(planar.find("curved blend: traverse above solid")==std::string::npos);
    Print fresh; fresh.apply(m,config);
    const auto clean=Test::gcode(fresh);
    auto moves=[](const std::string& text) {
        std::istringstream in(text); std::string line,out;
        while(std::getline(in,line)) if (line.rfind("G1",0)==0 || line.rfind("G0",0)==0 || line.rfind("G2",0)==0 || line.rfind("G3",0)==0)
            out+=line.substr(0,line.find(';'))+"\n";
        return out;
    };
    REQUIRE(moves(planar)==moves(clean));
}

TEST_CASE("Curved writer subdivides paths, compensates flow and avoids diagonal travel", "[curved-engine]")
{
    using namespace CurvedTransition;
    Ramp r{0.,20.,0.,20.,3.,.08,0.,3.,4.6};
    auto field=r.field(.2);
    GCodeWriter writer;
    writer.set_extruders({0}); writer.set_extruder(0);
    const Vec2d origin(100.,80.);
    writer.set_curved_blend(field,r,origin,2.,5.);
    writer.set_curved_path(2.,.45,.2);
    const auto approach=writer.travel_to_xyz(Vec3d(100.,90.,2.));
    REQUIRE(approach.find("Z6.6")!=std::string::npos);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(0.,10.,2.))<1e-9);
    const double before=writer.filament()->E();
    const auto extrusion=writer.extrude_to_xy(Vec2d(120.,90.),1.);
    REQUIRE(std::count(extrusion.begin(),extrusion.end(),'\n')>100);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(20.,10.,2.))<1e-9);
    REQUIRE(writer.filament()->E()>before);
    REQUIRE(writer.max_extrusion_z()>0.);
    REQUIRE_THROWS(writer.extrude_arc_to_xy(Vec2d(120.,100.),Vec2d(0.,5.),1.,true));
    REQUIRE_THROWS(writer.extrude_to_xyz(Vec3d(120.,100.,3.),1.));
}
