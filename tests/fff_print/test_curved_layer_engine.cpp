#include <catch2/catch_all.hpp>
#include "test_helpers.hpp"
#include "auto_planes_fixtures.hpp"
#include "test_utils.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/CurvedPlaneProgram.hpp"
#include "libslic3r/CurvedMesh.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
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
        {"wipe_on_loops","0"},{"wipe_before_external_loop","0"},
        {"ironing_type","no ironing"},{"fuzzy_skin","disabled_fuzzy"},{"zaa_enabled","0"},
        {"wave_overhangs","0"},{"make_overhang_printable","0"},
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

// Synthetic measured carriage, NOT a profile for any real printer. The second
// active tip has a different envelope, height, layer limits and XY offset.
DynamicPrintConfig shared_carriage_config(int active=0)
{
    auto c=curved_config();
    c.set_num_extruders(2);
    c.set_deserialize_strict({{"nozzle_diameter","0.4,0.4"},{"extruder_offset","0x0,12x-3"},
        {"min_layer_height","0.08,0.1"},{"max_layer_height","0.32,0.3"},
        {"extruder_printable_height","200,80"},{"nonplanar_shared_carriage","1"},
        {"nonplanar_state_nozzle_id","1,2"},{"nonplanar_state_complete","1,1"},
        {"nonplanar_state_source","synthetic state one;synthetic state two"},
        {"nonplanar_state_envelope_height","125,110"},{"nonplanar_state_gantry_height","14,13"},
        {"nonplanar_state_max_z","90,70"},{"nonplanar_state_angle_xpos","7,8"},
        {"nonplanar_state_angle_xneg","10,9"},{"nonplanar_state_angle_ypos","15,12"},
        {"nonplanar_state_angle_yneg","6,7"},{"machine_max_speed_z","10,0,5,0"},
        {"filament_map",active==0?"1":"2"},{"enable_filament_dynamic_map","0"}});
    c.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    c.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(0.,0.),Vec2d(200.,0.),Vec2d(200.,200.),Vec2d(0.,200.)},
        {Vec2d(20.,0.),Vec2d(190.,0.),Vec2d(190.,190.),Vec2d(20.,190.)}}));
    return c;
}

TriangleMesh holed_part()
{
    indexed_triangle_set mesh;
    for(int i=0;i<16;++i) {
        const double angle=i*2.*PI/16.;
        for(const auto& rz:std::vector<std::pair<double,double>>{{10.,0.},{10.,12.},{3.,0.},{3.,12.}})
            mesh.vertices.emplace_back(float(rz.first*std::cos(angle)),float(rz.first*std::sin(angle)),float(rz.second));
    }
    auto add=[&](int a,int b,int c){mesh.indices.emplace_back(a,b,c);};
    for(int i=0;i<16;++i) {
        const int a=4*i,b=4*((i+1)%16);
        add(a,b,b+1);add(a,b+1,a+1); // outer
        add(a+2,a+3,b+3);add(a+2,b+3,b+2); // bore
        add(a+1,b+1,b+3);add(a+1,b+3,a+3); // roof
        add(a,a+2,b+2);add(a,b+2,b); // bed
    }
    return TriangleMesh(std::move(mesh));
}

void add_plane(ModelObject& object,double height,double gx,double gy=0.)
{
    CutPlane p; p.point=Vec3d(0.,0.,height); p.normal=Vec3d(-gx,-gy,1.).normalized();
    object.cut_planes.enabled=true; object.cut_planes.planes.push_back(p);
}
}

TEST_CASE("Curved planes slice a curved roof without independent sections", "[curved-engine][planes]")
{
    Print print;Model model;
    const auto config=curved_config();
    Test::init_print({TriangleMesh(AutoPlanesFixtures::curved_top_block())},print,model,config);
    auto* object=model.objects.front();
    const auto before=object->volumes.front()->mesh().its.vertices;
    add_plane(*object,1.7,.02);
    print.apply(model,config);
    REQUIRE(print.objects().size()==1);
    REQUIRE_FALSE(print.objects().front()->is_segment());
    const auto output=Test::gcode(print);
    REQUIRE_FALSE(output.empty());
    REQUIRE(object->volumes.front()->mesh().its.vertices==before);
    REQUIRE(print.objects().front()->curved_field());
    REQUIRE_FALSE(print.objects().front()->curved_ramp());
    const auto* program=dynamic_cast<const CurvedTransition::PlaneProgram*>(print.objects().front()->curved_field().get());
    REQUIRE(program);
    REQUIRE(program->blends().size()==1);
}

TEST_CASE("Automatic slicing infers a bounded roof direction without changing the model", "[curved-engine][automatic]")
{
    auto config=curved_config();
    config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"curved_layer_blend","0"}});
    Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
    const auto vertices=model.objects.front()->volumes.front()->mesh().its.vertices;
    REQUIRE(print.validate().string.empty());
    const auto output=Test::gcode(print);
    const auto* object=print.objects().front();
    REQUIRE(object->curved_field());REQUIRE_FALSE(object->curved_ramp());
    REQUIRE_FALSE(model.objects.front()->has_cut_planes());
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==vertices);
    REQUIRE(output.find("; layer_strategy: Automatic selection: curved.")!=std::string::npos);
    REQUIRE(output.find("curved blend: traverse above solid")!=std::string::npos);
}

TEST_CASE("Automatic slicing keeps flat roofs planar and reselects after settings change", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,3.))},print,model,config);
    auto output=Test::gcode(print);
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(output.find("Automatic selection: planar.")!=std::string::npos);
    REQUIRE(output.find("curved blend: traverse above solid")==std::string::npos);
    Print tilted;Model tilted_model;Test::init_print({ramp(2.)},tilted,tilted_model,config);
    REQUIRE_FALSE(Test::gcode(tilted).empty());REQUIRE(tilted.objects().front()->curved_field());
    config.set_deserialize_strict({{"nonplanar_virtual_max_angle","0.1"}});
    tilted.apply(tilted_model,config);
    REQUIRE_FALSE(Test::gcode(tilted).empty());
    REQUIRE_FALSE(tilted.objects().front()->curved_field());
}

TEST_CASE("Automatic slicing preserves supports and ignores cut sections on planar fallback", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"enable_support","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,12.))},print,model,config);
    add_plane(*model.objects.front(),6.,.07);print.apply(model,config);
    REQUIRE(print.validate().string.empty());
    REQUIRE_FALSE(Test::gcode(print).empty());
    REQUIRE(print.objects().size()==1);REQUIRE_FALSE(print.objects().front()->is_segment());
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->config().enable_support.value);
    REQUIRE(model.objects.front()->cut_planes.planes.size()==1);
    REQUIRE(print.objects().front()->layer_strategy_decision().find("supports")!=std::string::npos);
}

TEST_CASE("Automatic slicing checks explicit plane transitions and rejects ambiguous manual rotation", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,18.))},print,model,config);
    add_plane(*model.objects.front(),4.,.035);add_plane(*model.objects.front(),11.,-.035);print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE(print.objects().front()->curved_field());
    model.objects.front()->cut_planes.planes.back().point.z()=4.1;print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->layer_strategy_decision().find("intersecting")!=std::string::npos);
    config.set_deserialize_strict({{"layer_rotation_b","2"}});print.apply(model,config);
    REQUIRE_FALSE(print.validate().string.empty());
}

TEST_CASE("Automatic slicing leaves multiple objects and sparse infill on their planar path", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    SECTION("several objects") {
        Print print;Model model;Test::init_print({ramp(2.),ramp(2.)},print,model,config);
        REQUIRE_FALSE(Test::gcode(print).empty());
        for (const auto* object:print.objects()) REQUIRE_FALSE(object->curved_field());
    }
    SECTION("sparse infill") {
        config.set_deserialize_strict({{"sparse_infill_density","15%"}});
        Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
        REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE_FALSE(print.objects().front()->curved_field());
        REQUIRE(print.objects().front()->layer_strategy_decision().find("100% infill")!=std::string::npos);
        REQUIRE(print.objects().front()->printing_region(0).config().sparse_infill_density.value==15.);
    }
}

TEST_CASE("Curved toolhead states use the selected physical nozzle and survive serialization", "[curved-engine][multi-nozzle]")
{
    auto dynamic=shared_carriage_config();
    DynamicPrintConfig reloaded;
    for (const auto& key:dynamic.keys()) reloaded.set_deserialize_strict(key,dynamic.option(key)->serialize());
    PrintConfig config;config.apply(reloaded,true);
    const auto first=CurvedTransition::ToolheadState::from_config(config,0);
    const auto second=CurvedTransition::ToolheadState::from_config(config,1);
    REQUIRE(first.max_z==90.);REQUIRE(second.max_z==70.);
    REQUIRE(second.envelope.height==110.);REQUIRE(second.envelope.angle[0]==8.);
    REQUIRE(second.min_layer==.1);REQUIRE(second.max_layer==.3);
    REQUIRE(first.z_speed==5.);REQUIRE(second.z_speed==5.);
    // Arbitrarily many states: the mechanism is not special-cased to two.
    dynamic.set_num_extruders(3);PrintConfig cloned;cloned.apply(dynamic,true);
    REQUIRE_THROWS_WITH(CurvedTransition::ToolheadState::from_config(cloned,2),Catch::Matchers::ContainsSubstring("identified"));
    config.nonplanar_state_nozzle_id.values[1]=1;
    REQUIRE_THROWS(CurvedTransition::ToolheadState::from_config(config,1));
}

TEST_CASE("Curved multi-nozzle printing requires recorded shared-carriage geometry and fixed mapping", "[curved-engine][multi-nozzle]")
{
    PrintConfig config;config.apply(shared_carriage_config(1),true);
    SECTION("unknown inactive hardware") {config.nonplanar_state_complete.values[1]=false;}
    SECTION("missing source") {config.nonplanar_state_source.values[1].clear();}
    SECTION("independent carriage") {config.nonplanar_shared_carriage.value=false;}
    SECTION("automatic assignment") {config.filament_map_mode.value=fmmAutoForFlush;}
    SECTION("dynamic mapping") {REQUIRE_THROWS(CurvedTransition::ToolheadState::from_config(config,1,true));return;}
    SECTION("missing active offset") {config.extruder_offset.values.resize(1);}
    SECTION("unknown side angle") {config.nonplanar_state_angle_xneg.values[1]=0.;}
    SECTION("unknown active layer limit") {config.min_layer_height.values.resize(1);}
    SECTION("unknown Z bound") {config.nonplanar_state_max_z.values[1]=0.;}
    REQUIRE_THROWS(CurvedTransition::ToolheadState::from_config(config,1));
}

TEST_CASE("Automatic slicing supports either fixed nozzle on a synthetic shared carriage", "[curved-engine][multi-nozzle][automatic]")
{
    const int active=GENERATE(0,1);
    auto config=shared_carriage_config(active);
    config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"curved_layer_blend","0"}});
    Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));print.apply(model,config);
    REQUIRE(print.validate().string.empty());
    const auto output=Test::gcode(print);
    REQUIRE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->curved_toolhead_state().nozzle==size_t(active));
    REQUIRE(output.find("Automatic selection: curved.")!=std::string::npos);
    // Changing the active state's Z bound must trigger slicing again.
    config.set_deserialize_strict({{"nonplanar_state_max_z",active==0?"4,70":"90,4"}});print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE_FALSE(print.objects().front()->curved_field());
}

TEST_CASE("Automatic slicing rejects unmeasured states and out of range active footprints", "[curved-engine][multi-nozzle][automatic]")
{
    auto config=shared_carriage_config(1);config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    SECTION("unmeasured") {config.set_deserialize_strict({{"nonplanar_state_complete","1,0"}});}
    SECTION("narrow active area") {config.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(0.,0.),Vec2d(200.,0.),Vec2d(200.,200.),Vec2d(0.,200.)},
        {Vec2d(150.,0.),Vec2d(190.,0.),Vec2d(190.,190.),Vec2d(150.,190.)}}));}
    Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));print.apply(model,config);
    const_cast<PrintObject*>(print.objects().front())->slice();REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->layer_strategy_decision().find("Automatic selection: planar.")!=std::string::npos);
}

TEST_CASE("Curved writer keeps the active offset and refuses switching before mutating the nozzle", "[curved-engine][multi-nozzle]")
{
    PrintConfig config;config.apply(shared_carriage_config(1),true);
    config.filament_map.values={2,1};config.filament_diameter.values={1.75,1.75};
    GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0,1});writer.set_extruder(0);
    REQUIRE(writer.filament()->extruder_id()==1);
    CurvedTransition::Ramp ramp{0.,20.,0.,20.,3.,.08,0.,3.,4.6};auto field=ramp.field(.2);
    const Vec2d placed(100.,80.);
    const Vec2d origin=placed-writer.extruder_offset_xy().head<2>();
    REQUIRE_THROWS(writer.set_curved_blend(field,ramp.domain(),origin,2.,5.,0));
    writer.set_curved_blend(field,ramp.domain(),origin,2.,5.,1);writer.set_curved_path(2.,.45,.2);
    writer.travel_to_xyz(Vec3d(origin.x(),origin.y()+10.,2.));
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(origin.x()+20.,origin.y()+10.),1.).empty());
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(20.,10.,2.))<1e-9);
    REQUIRE(std::abs(writer.get_position().x()+12.-(placed.x()+20.))<1e-9);
    REQUIRE(std::abs(writer.get_position().y()-3.-(placed.y()+10.))<1e-9);
    const auto position=writer.get_position();
    REQUIRE_THROWS(writer.toolchange(1,0));
    REQUIRE(writer.filament()->extruder_id()==1);REQUIRE(writer.get_position()==position);
}

TEST_CASE("Curved plane project reload preserves world positions when the volume origin moves", "[curved-engine][planes][3mf]")
{
    Model model;
    auto* object=model.add_object();
    auto* volume=object->add_volume(TriangleMesh(its_make_cube(20.,20.,12.)));
    auto* instance=object->add_instance();
    instance->set_offset(Vec3d(128.,128.,0.));
    REQUIRE(volume->get_offset().norm()>1.);
    add_plane(*object,6.,.035);
    const Vec3d world_point=instance->get_matrix()*object->cut_planes.planes.front().point;
    ScopedTemporaryDir backup("curved_3mf");model.set_backup_path(backup.string());
    ScopedTemporaryFile temp(".3mf");const std::string path=temp.string();
    auto config=curved_config();
    PlateData plate;plate.plate_index=0;
    StoreParams params;
    params.path=path.c_str();params.model=&model;params.config=&config;
    params.plate_data_list.push_back(&plate);
    params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded;DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;
    bool is_bbl=false,is_orca=false;Semver version;
    REQUIRE(load_bbs_3mf(path.c_str(),&loaded_config,&context,&loaded,&plates,&presets,
                        &is_bbl,&is_orca,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    release_PlateData_list(plates);
    REQUIRE(loaded.objects.size()==1);
    const auto* restored=loaded.objects.front();
    REQUIRE(restored->cut_planes.planes.size()==1);
    const Vec3d restored_world=restored->instances.front()->get_matrix()*restored->cut_planes.planes.front().point;
    REQUIRE((restored_world-world_point).norm()<1e-6);
    REQUIRE((restored->cut_planes.planes.front().normal-object->cut_planes.planes.front().normal).norm()<1e-6);
    Print print;print.apply(loaded,config);
    REQUIRE(print.validate().string.empty());
    REQUIRE_FALSE(Test::gcode(print).empty());
}

TEST_CASE("Curved plane mesh mapping keeps a through hole and bounded facet error", "[curved-engine][planes][mesh]")
{
    using namespace CurvedTransition;
    const auto mesh=holed_part().its;
    PlaneProgram program({-10.,10.,-10.,10.,12.,.07},.2,.2,{{6.,.07,0.}});
    const auto mapped=map_mesh(mesh,program,[]{});
    REQUIRE(mapped.indices.size()>mesh.indices.size());
    REQUIRE(its_num_open_edges(mapped)==0);
    // Genus-one boundary: refinement preserves the bore's Euler characteristic.
    std::set<std::pair<int,int>> edges;
    for(const auto& f:mapped.indices) for(int i=0;i<3;++i) {
        const auto edge=std::minmax(f[i],f[(i+1)%3]);edges.emplace(edge.first,edge.second);
    }
    REQUIRE(int64_t(mapped.vertices.size())-int64_t(edges.size())+int64_t(mapped.indices.size())==0);
    for(const auto& f:mapped.indices) {
        Vec3d p=Vec3d::Zero(); double physical=0.;
        for(int i=0;i<3;++i) {
            const Vec3d q=mapped.vertices[f[i]].cast<double>();p+=q/3.;
            physical+=program.world_z(q.x(),q.y(),q.z())/3.;
        }
        REQUIRE(std::abs(program.world_z(p.x(),p.y(),p.z())-physical)<=.0101);
    }
    Print print;Model model; const auto config=curved_config();
    Test::init_print({holed_part()},print,model,config);
    add_plane(*model.objects.front(),6.,.07);
    print.apply(model,config); print.process();
    REQUIRE(print.objects().size()==1);
    const auto* object=print.objects().front();
    size_t holes=0;
    for(const auto* layer:object->layers()) for(const auto& poly:layer->lslices) holes+=poly.holes.size();
    REQUIRE(holes>object->layers().size()/2);
    ScopedTemporaryFile temp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(),&result,nullptr);
    const double bound=object->curved_domain()->top_max;
    REQUIRE(bound>12.);
    double actual_top=0.;size_t traverses=0;
    for (size_t i=1;i<result.moves.size();++i) {
        const auto& move=result.moves[i];const auto& prior=result.moves[i-1];
        if (move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
            actual_top=std::max(actual_top,double(move.position.z()));
            REQUIRE(move.position.z()<=bound+.002);
        }
        if (move.type==EMoveType::Travel && actual_top>0. &&
            (move.position.head<2>()-prior.position.head<2>()).norm()>.001f) {
            ++traverses;
            REQUIRE(move.position.z()>=bound+1.999);
            REQUIRE(prior.position.z()>=bound+1.999);
        }
    }
    // This flat CAD roof crosses tilted layer contours: midpoint sampling
    // actually deposits a fraction of a layer above its nominal 12 mm top.
    REQUIRE(actual_top>12.01);
    REQUIRE(traverses>0);
    auto restricted=config;
    restricted.set_deserialize_strict({{"nonplanar_virtual_max_z","14.01"}});
    print.apply(model,restricted);
    REQUIRE_THROWS(Test::gcode(print));
}

TEST_CASE("Curved engine blends two plane changes and refuses intersecting branches", "[curved-engine][planes]")
{
    Print print;Model model; const auto config=curved_config();
    Test::init_print({TriangleMesh(its_make_cube(20.,20.,18.))},print,model,config);
    add_plane(*model.objects.front(),4.,.035);
    add_plane(*model.objects.front(),11.,-.035);
    print.apply(model,config); print.process();
    REQUIRE(print.objects().size()==1);
    const auto* program=dynamic_cast<const CurvedTransition::PlaneProgram*>(print.objects().front()->curved_field().get());
    REQUIRE(program); REQUIRE(program->blends().size()==2);
    REQUIRE_FALSE(Test::gcode(print).empty());
    model.objects.front()->cut_planes.planes.back().point.z()=4.1;
    print.apply(model,config);
    REQUIRE_THROWS(Test::gcode(print));
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
    const std::regex header(R"(; max_z_height: (-?\d*\.?\d+))");
    std::smatch reported;REQUIRE(std::regex_search(gcode,reported,header));
    REQUIRE(std::stod(reported[1])>=maxz-.005);
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
    writer.set_curved_blend(field,r.domain(),origin,2.,5.);
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
    // Classic walls can generate narrow gap-fill paths. Keep upstream's area
    // convention and bound the maximum flow even when its height maximum is
    // not where the concave area expression reaches its maximum.
    writer.set_curved_path(2.,.09,.2);
    REQUIRE(writer.curved_max_flow_scale()>1.);
    writer.travel_to_xyz(Vec3d(100.,90.,2.));
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(120.,90.),1.).empty());
    REQUIRE_THROWS(writer.set_curved_path(2.,.02,.2));
    const auto layer_change=writer.travel_to_z(2.2);
    REQUIRE(layer_change.find("F300")!=std::string::npos);
}
