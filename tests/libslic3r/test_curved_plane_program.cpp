#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedPlaneProgram.hpp"
#include "libslic3r/CurvedRamp.hpp"

using namespace Slic3r::CurvedTransition;

TEST_CASE("Plane field blends successive directions without crossing layers", "[curved-blend][planes]")
{
    ModelDomain d{-10.,10.,-8.,8.,20.,.07};
    PlaneProgram p(d,.2,.2,{{5.,.07,0.},{13.,-.04,.04}});
    REQUIRE(p.blends().size()==2);
    REQUIRE(p.inverse_hessian_bound()>0.);
    // A single intermediate layer curves in XY, rather than being one tilted
    // flat plane. Its midpoint is not the chord between opposing sides.
    const auto& blend=p.blends().front();
    const double w=(blend.lo+blend.hi)*.5;
    REQUIRE(std::abs(p.world_z(-10.,0.,w)+p.world_z(10.,0.,w)-2.*p.world_z(0.,0.,w))>.001);
    for(double x:{-10.,-5.,0.,10.}) for(double y:{-8.,0.,8.}) {
        REQUIRE(p.world_z(x,y,.2)==.2);
        double last=.2;
        for(int i=1;i<=200;++i) {
            const double w=.2+i*.1,z=p.world_z(x,y,w);
            REQUIRE(z>last);
            REQUIRE(z-last>=.05-1e-9);
            REQUIRE(z-last<=.15+1e-9);
            REQUIRE(std::abs(p.slice_z(x,y,z)-w)<1e-10);
            const auto g=p.gradient(x,y,w);
            REQUIRE(std::hypot(g.dx,g.dy)<=p.slope_bound()+1e-10);
            const double eps=1e-4;
            if(x>-9. && x<9.) REQUIRE(std::abs((p.world_z(x+eps,y,w)-p.world_z(x-eps,y,w))/(2.*eps)-g.dx)<1e-8);
            last=z;
        }
    }
    for(const auto& b:p.blends()) {
        const auto a=p.gradient(0.,0.,b.lo),z=p.gradient(0.,0.,b.hi);
        REQUIRE(std::abs(a.dx-b.from_x)<1e-12);
        REQUIRE(std::abs(z.dx-b.from_x-b.delta_x)<1e-12);
        const double epsilon=1e-5;
        REQUIRE(std::abs(p.gradient(0.,0.,b.lo+epsilon).dx-a.dx)<1e-12);
        REQUIRE(std::abs(p.gradient(0.,0.,b.hi-epsilon).dx-z.dx)<1e-12);
    }
    Ramp scheduler{};
    const auto layers=scheduler.schedule(p,.2,.2,.08,.28);
    REQUIRE(std::abs(layers.back()-p.reference_top_mm())<1e-10);
}

TEST_CASE("Plane field refuses infeasible directions and bounds", "[curved-blend][planes]")
{
    const ModelDomain d{-10.,10.,-8.,8.,10.,.07};
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{.1,.07,0.}}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{3.,.5,0.}}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{3.,.07,0.},{3.1,-.07,0.}}));
    PlaneProgram p(d,.2,.2,{{4.,.07,0.}});
    REQUIRE_THROWS(p.world_z(11.,0.,5.));
}
