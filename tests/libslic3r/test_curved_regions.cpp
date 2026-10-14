#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedRegions.hpp"
using namespace Slic3r::CurvedTransition;

TEST_CASE("Independent roof regions preserve planar bridge zones and invert continuously", "[curved-blend][regions]")
{
    Regions f({-20.,20.,-10.,10.,12.,0.},{{-8.,0.,4.,3.,4.,.04,0.},{8.,0.,4.,5.,6.,0.,-.04}},.08);
    REQUIRE(f.identity_on_box(-20.,20.,-10.,10.,0.,3.));
    REQUIRE(f.identity_on_box(-2.,2.,-10.,10.,0.,12.));
    REQUIRE_FALSE(f.identity_on_box(-10.,-6.,-1.,1.,3.,4.));
    REQUIRE(f.world_z(0.,0.,10.)==10.);
    REQUIRE(f.gradient(-8.,0.,8.).dx==Catch::Approx(.04));
    REQUIRE(f.gradient(8.,0.,8.).dy==Catch::Approx(-.04));
    for(double x=-20.;x<=20.;x+=.5) for(double y=-5.;y<=5.;y+=.5) for(double z=0.;z<=12.;z+=.4) {
        const double w=f.slice_z(x,y,z);
        REQUIRE(std::abs(f.world_z(x,y,w)-z)<1e-10);
        REQUIRE(std::abs(w-z)<=f.max_displacement_mm()+1e-12);
        const auto g=f.gradient(x,y,w);
        REQUIRE(std::hypot(g.dx,g.dy)<=f.slope_bound()+1e-10);
        const double dz=(f.slice_z(x,y,z+1e-5)-f.slice_z(x,y,z-1e-5))/2e-5;
        REQUIRE(dz>=f.jacobian_min()-1e-8);REQUIRE(dz<=f.jacobian_max()+1e-8);
    }
    f.require_spacing(.2,.08,.32);
    REQUIRE_THROWS(Regions({-10.,10.,-10.,10.,12.,0.},{{0.,0.,4.,3.,3.01,.04,0.}},.08));
    REQUIRE_THROWS(Regions({-10.,10.,-10.,10.,12.,0.},{{0.,0.,4.,3.,4.,.04,0.},{2.,0.,4.,3.,4.,0.,.04}},.08));
}

TEST_CASE("Regional selection retains useful roofs above protected undersides", "[curved-blend][regions][automatic]")
{
    std::vector<Ramp::Triangle> faces{{{{-10.,-10.,10.},{10.,-10.,11.},{0.,10.,10.5}}},
        {{{-10.,-10.,3.},{0.,10.,3.},{10.,-10.,3.}}}};
    const auto p=select_regions(faces,.2,.2,.08,1.,false);
    REQUIRE_FALSE(p.empty());
    Regions f({-10.,10.,-10.,10.,11.,0.},p,.08);
    REQUIRE(f.identity_on_box(-10.,10.,-10.,10.,0.,3.6));
    REQUIRE(f.slope_bound()<=.08);
    // Its only useful broad patch cannot fit inside this thin sparse top shell.
    REQUIRE(select_regions(faces,.2,.2,.08,1.,true).empty());
    for(auto& v:faces[1]) v[2]=10.5;
    REQUIRE(select_regions(faces,.2,.2,.08,1.,false).empty());
}

TEST_CASE("Regional sparse selection ranks whole roof alignment instead of patch centre alone", "[curved-blend][regions][automatic]")
{
    std::vector<Ramp::Triangle> narrow{{{{0.,0.,10.},{2.,0.,10.1},{2.,2.,10.1}}},
        {{{0.,0.,10.},{2.,2.,10.1},{0.,2.,10.}}},
        {{{0.,0.,3.},{2.,2.,3.},{2.,0.,3.}}},{{{0.,0.,3.},{0.,2.,3.},{2.,2.,3.}}}};
    const auto p=select_regions(narrow,.2,.2,.08,1.,true);
    REQUIRE_FALSE(p.empty());
    Regions f({0.,2.,0.,2.,10.1,0.},p,.08);
    double before=0.,after=0.;
    // Independent denser XY integration across the complete roof, including taper.
    for(int i=0;i<100;++i) for(int j=0;j<100;++j) {
        const double x=(i+.5)*.02,y=(j+.5)*.02;
        const auto g=f.logical_roof_gradient(x,y,10.+.05*x,.05,0.);
        before+=.05*.05;after+=g.dx*g.dx+g.dy*g.dy;
    }
    REQUIRE(after<before);
    REQUIRE(f.identity_on_box(0.,2.,0.,2.,0.,9.));
}
