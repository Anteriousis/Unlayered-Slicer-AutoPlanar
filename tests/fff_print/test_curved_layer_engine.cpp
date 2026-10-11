#include <catch2/catch_all.hpp>
#include "test_helpers.hpp"
#include "auto_planes_fixtures.hpp"
#include "test_utils.hpp"
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
        {"filament_adaptive_volumetric_speed","0"},{"elefant_foot_compensation","0"}
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
    // Verify every native deposition has XYZ, stays above the foundation and
    // below the true roof, including paths running across X and off-axis infill.
    std::istringstream in(gcode); std::string line;
    size_t extrusions=0; double minz=1e9,maxz=0.;
    const std::regex zword(R"( Z(-?\d*\.?\d+))");
    while(std::getline(in,line)) {
        const auto code=line.substr(0,line.find(';'));
        if (code.rfind("G0",0)==0 || code.rfind("G1",0)==0) {
            REQUIRE(code.find(" B")==std::string::npos);
            REQUIRE(code.find(" C")==std::string::npos);
        }
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

TEST_CASE("Curved blend Preview receives the same physical deposition and bounded travel", "[curved-engine][preview]")
{
    Print print; Model model;
    Test::init_print({ramp()},print,model,curved_config());
    print.process();
    ScopedTemporaryFile temp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(),&result,nullptr);
    REQUIRE(result.virtual_5axis);
    REQUIRE(result.tool_axis_fixed);
    REQUIRE(result.fixed_tool_axis.isApprox(Vec3f(0.f,0.f,1.f),1e-6f));
    REQUIRE(result.tool_axes.size()>1);
    const auto* object=print.objects().front();
    const auto& roof=*object->curved_ramp();
    const auto& field=*object->curved_field();
    const Vec2d shift=unscale(object->instances().front().shift);
    size_t deposits=0, traverses=0; double maxz=0., minh=1e9, maxh=0.;
    for (size_t i=1;i<result.moves.size();++i) {
        const auto& move=result.moves[i];
        const auto& prior=result.moves[i-1];
        if (move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
            ++deposits; maxz=std::max(maxz,double(move.position.z()));
            minh=std::min(minh,double(move.height)); maxh=std::max(maxh,double(move.height));
            REQUIRE(move.position.z()>=.199);
            const double x=move.position.x()-shift.x(),y=move.position.y()-shift.y();
            REQUIRE(move.position.z()<=roof.roof(x,y)+.002);
            REQUIRE(move.tool_axis_id<result.tool_axes.size());
            const Vec3f normal=result.tool_axes[move.tool_axis_id];
            REQUIRE(move.delta_extruder>0.);
            const auto grad=field.gradient(x,y,field.slice_z(x,y,move.position.z()));
            REQUIRE(std::hypot(grad.dx,grad.dy)<=roof.slope()+1e-6);
            const Vec3f expected=Vec3f(float(-grad.dx),float(-grad.dy),1.f).normalized();
            REQUIRE((normal-expected).norm()<.002f); // midpoint tag versus rounded endpoint
        }
        if (move.type==EMoveType::Travel &&
            (move.position.head<2>()-prior.position.head<2>()).norm()>.001f &&
            deposits>0) {
            ++traverses;
            REQUIRE(prior.position.z()>=roof.top_max+1.999);
            REQUIRE(move.position.z()>=roof.top_max+1.999);
            REQUIRE(std::abs(move.position.z()-prior.position.z())<.002);
        }
    }
    REQUIRE(deposits>1000);
    REQUIRE(traverses>0);
    REQUIRE(minh>=print.config().min_layer_height.get_at(0)-.002);
    REQUIRE(maxh<=print.config().max_layer_height.get_at(0)+.002);
    REQUIRE(maxh-minh>.04); // physical spacing changes through the blend
    REQUIRE(maxz>field.reference_top_mm()+.1);
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
    SECTION("draft shield outside the curved domain") {
        auto config=curved_config(); config.set_deserialize_strict({{"draft_shield","enabled"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("surface wipe is not a retracted travel") {
        auto config=curved_config(); config.set_deserialize_strict({{"wipe_on_loops","1"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
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

TEST_CASE("Curved slicing keeps instance placement and mirrored cardinal slopes", "[curved-engine][transform]")
{
    for (bool mirror:{false,true}) for (int quarter=0;quarter<4;++quarter) {
        CAPTURE(mirror,quarter);
        auto config=curved_config();
        Print print; Model model;
        Test::init_print({ramp(4.)},print,model,config);
        auto* object=model.objects.front();
        if (mirror) object->volumes.front()->set_mirror(X,-1.);
        object->instances.front()->set_rotation(Z,quarter*PI*.5);
        object->instances.front()->set_offset(Vec3d(40.,50.,0.));
        object->invalidate_bounding_box();
        print.apply(model,config);
        REQUIRE(print.validate().string.empty());
        print.process();
        const auto* sliced=print.objects().front();
        REQUIRE(sliced->curved_field());
        const auto& roof=*sliced->curved_ramp();
        const double sign=mirror?-1.:1.;
        REQUIRE(std::abs(roof.gx-sign*std::tan(4.*PI/180.)*std::cos(quarter*PI*.5))<1e-6);
        REQUIRE(std::abs(roof.gy-sign*std::tan(4.*PI/180.)*std::sin(quarter*PI*.5))<1e-6);
        REQUIRE(std::abs(roof.top_min-3.)<1e-5);
        REQUIRE(std::abs(roof.top_max-(3.+20.*std::tan(4.*PI/180.)))<1e-5);
        REQUIRE(std::abs(sliced->curved_field()->world_z(roof.xmin,roof.ymin,sliced->layers().back()->print_z)-
                         roof.roof(roof.xmin,roof.ymin))<1e-8);
    }
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
    const double before=writer.filament()->used_filament();
    const auto extrusion=writer.extrude_to_xy(Vec2d(120.,90.),1.);
    REQUIRE(std::count(extrusion.begin(),extrusion.end(),'\n')>100);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(20.,10.,2.))<1e-9);
    REQUIRE(writer.filament()->used_filament()>before);
    // Independent fine-grid volume integral. A unit nominal E along 20 mm
    // should deliver the mapped bead's area times physical path length.
    double expected=0.;
    const double dx=20./10000;
    for (int i=0;i<10000;++i) {
        const double x=(i+.5)*dx;
        const double h=(field->world_z(x,10.,2.)-field->world_z(x,10.,1.8))/
            std::sqrt(1.+std::pow(field->gradient(x,10.,2.).dx,2));
        const double dz=field->world_z(x+dx*.5,10.,2.)-field->world_z(x-dx*.5,10.,2.);
        expected+=Ramp::bead_area(.45,h)/Ramp::bead_area(.45,.2)*std::hypot(dx,dz)/20.;
    }
    REQUIRE(std::abs((writer.filament()->used_filament()-before)-expected)<.0001);
    REQUIRE(writer.max_extrusion_z()>0.);
    REQUIRE_THROWS(writer.extrude_arc_to_xy(Vec2d(120.,100.),Vec2d(0.,5.),1.,true));
    REQUIRE_THROWS(writer.extrude_to_xyz(Vec3d(120.,100.,3.),1.));
    REQUIRE_THROWS(writer.extrude_to_xy(Vec2d(120.,100.),0.,"surface wipe",true));
    const auto layer_change=writer.travel_to_z(2.2);
    REQUIRE(layer_change.find("F300")!=std::string::npos);
}
