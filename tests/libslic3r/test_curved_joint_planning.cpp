#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedJointPlanning.hpp"
using namespace Slic3r::CurvedTransition;

TEST_CASE("Joint underside and roof fields share a monotone bounded coordinate map", "[curved-blend][joint]")
{
    const ModelDomain domain{-10.,10.,-10.,10.,30.,0.};
    auto under=std::make_shared<UndersideProgram>(domain,.2,.2,PlaneProgram::Target{7.,.015,0.},.08);
    auto roof=std::make_shared<Regions>(domain,std::vector<Regions::Patch>{{0.,0.,5.,15.,17.,0.,-.02}},.04);
    JointField joint(under,roof,.08);
    REQUIRE(joint.maps_supports());
    REQUIRE(joint.identity_on_box(-10.,10.,-10.,10.,0.,.2));
    REQUIRE(joint.affine_on_box(-2.,2.,-2.,2.,10.,11.));
    REQUIRE_FALSE(joint.affine_on_box(-2.,2.,-2.,2.,15.,18.));
    for(double x=-9.;x<=9.;x+=1.5) for(double y=-9.;y<=9.;y+=1.5) for(double z=.1;z<=30.;z+=.65) {
        const double w=joint.slice_z(x,y,z),h=1e-4;
        REQUIRE(joint.world_z(x,y,w)==Catch::Approx(z).margin(1e-10));
        const auto g=joint.gradient(x,y,w);
        REQUIRE(std::hypot(g.dx,g.dy)<=joint.slope_bound()+1e-9);
        REQUIRE(g.dx==Catch::Approx((joint.world_z(x+h,y,w)-joint.world_z(x-h,y,w))/(2*h)).margin(1e-7));
        REQUIRE(g.dy==Catch::Approx((joint.world_z(x,y+h,w)-joint.world_z(x,y-h,w))/(2*h)).margin(1e-7));
        const double j=(joint.slice_z(x,y,z+h)-joint.slice_z(x,y,z-h))/(2*h);
        REQUIRE(j>=joint.jacobian_min()-1e-7);
        REQUIRE(j<=joint.jacobian_max()+1e-7);
        REQUIRE(std::abs(z-w)<=joint.max_displacement_mm()+1e-9);
        for(const std::array<double,3> d:std::vector<std::array<double,3>>{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.},{.6,0.,.8}}) {
            const double second=(joint.slice_z(x+h*d[0],y+h*d[1],z+h*d[2])-2*w+
                joint.slice_z(x-h*d[0],y-h*d[1],z-h*d[2]))/(h*h);
            REQUIRE(std::abs(second)<=joint.inverse_hessian_bound_box(x-h,x+h,y-h,y+h,z-h,z+h)+1e-5);
        }
    }
    REQUIRE_THROWS(JointField(under,roof,.02));
    joint.require_spacing(.2,.08,.32);
}

TEST_CASE("Joint planning compares full roof and underside benefit with generated costs", "[curved-blend][joint]")
{
    SurfaceBenefit planar{10.,20.,100.},good{5.,10.,100.},bad{12.,22.,100.};
    GeneratedCost cost{100.,20.,500.,60.,20};
    REQUIRE(planning_score(planar,planar,cost,cost)==0.);
    const double gain=planning_score(planar,good,cost,cost);
    REQUIRE(gain>0.);
    auto expensive=cost;expensive.motion_seconds=600.;
    REQUIRE(planning_score(planar,good,cost,expensive)<0.);
    REQUIRE(planning_score(planar,bad,cost,cost)<0.);
    auto saved=cost;saved.support_mm3=10.;
    REQUIRE(planning_score(planar,good,cost,saved)>gain);
    const std::vector<Ramp::Triangle> roof{{{{0.,0.,20.},{8.,0.,20.4},{8.,8.,20.4}}},
        {{{0.,0.,20.},{8.,8.,20.4},{0.,8.,20.}}}};
    const auto before=surface_benefit(roof,nullptr,30.);
    PlaneProgram aligned({0.,8.,0.,8.,21.,0.},.2,.2,{{5.,.05,0.}},.1);
    const auto after=surface_benefit(roof,&aligned,30.);
    REQUIRE(after.roof_error<before.roof_error*.001);
    REQUIRE(after.roof_area_mm2==before.roof_area_mm2);
    REQUIRE(after.underside_deficit_mm2==0.);
}

TEST_CASE("Region decision metadata is bounded and survives an imported preview", "[curved-blend][joint][metadata]")
{
    RegionDecision a{3,0,true,12.,-4.,20.,6.,2.5,.15,64.,20.,"Joint roof: finish gain exceeds added motion cost"},b;
    REQUIRE(RegionDecision::decode(a.encode(),b));
    REQUIRE(b.encode()==a.encode());
    REQUIRE_FALSE(RegionDecision::decode("v9 "+a.encode().substr(3),b));
    REQUIRE_FALSE(RegionDecision::decode(std::string(1025,'a'),b));
    auto invalid=a;invalid.radius=-1.;REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.x=INFINITY;REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.reason="line\nbreak";REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.selected=false;invalid.reason="Planar: generated support contact overlaps this roof";
    REQUIRE(RegionDecision::decode(invalid.encode(),b));REQUIRE_FALSE(b.selected);
}
