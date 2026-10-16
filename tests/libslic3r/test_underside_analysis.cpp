#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/UndersideAnalysis.hpp"
using namespace Slic3r::CurvedTransition;

namespace {
Ramp::Triangle underside(double angle,double azimuth=0.,double z=4.)
{
    const double pi=3.14159265358979323846,g=std::tan(angle*pi/180.);
    const double x=std::cos(azimuth*pi/180.),y=std::sin(azimuth*pi/180.);
    // Downward winding; a positive angle rises toward the chosen azimuth.
    return {{{0.,0.,z},{-10.*y,10.*x,z},{10.*x,10.*y,z+10.*g}}};
}
}

TEST_CASE("Underside analysis excludes bed contact and upward or vertical faces", "[curved-blend][undersides]")
{
    const std::vector<Ramp::Triangle> faces{
        {{{0.,0.,0.},{0.,10.,0.},{10.,0.,0.}}},
        {{{0.,0.,4.},{10.,0.,4.},{0.,10.,4.}}},
        {{{0.,0.,0.},{10.,0.,0.},{0.,0.,4.}}}};
    const auto r=analyze_undersides(faces,.08,30.);
    REQUIRE(r.facets==0);REQUIRE(r.projected_area_mm2==0.);
    REQUIRE_FALSE(r.has_candidate);REQUIRE(r.candidates_checked==0);
    REQUIRE(underside_analysis_summary(r).find("no elevated underside")!=std::string::npos);
}

TEST_CASE("Underside analysis clips a crossing triangle to the bed", "[curved-blend][undersides]")
{
    // Above Z=0, the triangle has 3/4 of its XY area. Include the small
    // explicit bed tolerance in the independent similar-triangles calculation.
    const Ramp::Triangle t{{{0.,0.,-1.},{0.,10.,1.},{10.,0.,1.}}};
    const auto r=analyze_undersides({t},0.,30.);
    const double removed_side=(1.+2e-5)/2.;
    REQUIRE(r.facets==1);
    REQUIRE(r.projected_area_mm2==Catch::Approx(50.*(1.-removed_side*removed_side)).margin(1e-10));
    REQUIRE_FALSE(r.has_candidate);
}

TEST_CASE("Underside candidates use the improving signed direction and stay in the angle bound", "[curved-blend][undersides]")
{
    const double allowed=std::tan(5.*3.14159265358979323846/180.);
    const auto r=analyze_undersides({underside(20.)},allowed,30.);
    REQUIRE(r.has_candidate);REQUIRE(r.facets==1);
    REQUIRE(r.minimum_angle_deg==Catch::Approx(20.));
    REQUIRE(r.projected_area_mm2==Catch::Approx(50.));
    const double area=50./std::cos(20.*3.14159265358979323846/180.);
    REQUIRE(r.planar.angular_deficit_mm2==Catch::Approx(area/3.));
    REQUIRE(r.best.angular_deficit_mm2==Catch::Approx(area/6.));
    REQUIRE(r.planar.minimum_angle_deg==Catch::Approx(20.));
    REQUIRE(r.best.minimum_angle_deg==Catch::Approx(25.));
    REQUIRE(r.best.gx==Catch::Approx(-allowed));REQUIRE(std::abs(r.best.gy)<1e-10);
    REQUIRE(r.best.worsened_mm2==0.);REQUIRE(r.best.improved_mm2==Catch::Approx(area));
    REQUIRE(std::hypot(r.best.gx,r.best.gy)<=allowed+1e-12);
    REQUIRE(r.candidates_checked<=128);
}

TEST_CASE("Underside search includes diagonals and does not mistake nozzle angle for layer angle", "[curved-blend][undersides]")
{
    const double allowed=std::tan(5.*3.14159265358979323846/180.);
    const auto r=analyze_undersides({underside(20.,37.)},allowed,30.);
    REQUIRE(r.has_candidate);
    REQUIRE(r.best.gx==Catch::Approx(-allowed*std::cos(37.*3.14159265358979323846/180.)));
    REQUIRE(r.best.gy==Catch::Approx(-allowed*std::sin(37.*3.14159265358979323846/180.)));
    REQUIRE(underside_analysis_summary(r).find("Candidate not applied")!=std::string::npos);
    REQUIRE(underside_analysis_summary(r).find("Supports unchanged")!=std::string::npos);
}

TEST_CASE("Underside ranking exposes regressions on opposing overhangs", "[curved-blend][undersides]")
{
    auto dominant=underside(20.);for(auto& p:dominant) {p[0]*=2.;p[1]*=2.;p[2]=4.+(p[2]-4.)*2.;}
    const auto r=analyze_undersides({dominant,underside(20.,180.)},.08,30.);
    const double area=50./std::cos(20.*3.14159265358979323846/180.);
    REQUIRE(r.has_candidate);REQUIRE(r.best.worsened_mm2==Catch::Approx(area));
    REQUIRE(r.best.improved_mm2==Catch::Approx(4.*area));
    REQUIRE(r.best.worst_regression_deg>4.);
    REQUIRE(r.best.angular_deficit_mm2<r.planar.angular_deficit_mm2);
}

TEST_CASE("Underside angular costs are area weighted across mesh subdivision", "[curved-blend][undersides]")
{
    const auto t=underside(20.,37.);Ramp::Vertex mid{};
    for(size_t k=0;k<3;++k) mid[k]=(t[1][k]+t[2][k])*.5;
    const auto a=analyze_undersides({t},.08,30.);
    const auto b=analyze_undersides({{t[0],t[1],mid},{t[0],mid,t[2]}},.08,30.);
    REQUIRE(a.projected_area_mm2==Catch::Approx(b.projected_area_mm2));
    REQUIRE(a.best.angular_deficit_mm2==Catch::Approx(b.best.angular_deficit_mm2));
    REQUIRE(a.best.gx==Catch::Approx(b.best.gx));REQUIRE(a.best.gy==Catch::Approx(b.best.gy));
}

TEST_CASE("Underside directions and costs do not depend on object translation", "[curved-blend][undersides]")
{
    auto t=underside(20.,37.);
    const auto a=analyze_undersides({t},.08,30.);
    for(auto& p:t) {p[0]+=150.;p[1]-=80.;p[2]+=10.;}
    const auto b=analyze_undersides({t},.08,30.);
    REQUIRE(a.minimum_angle_deg==Catch::Approx(b.minimum_angle_deg));
    REQUIRE(a.projected_area_mm2==Catch::Approx(b.projected_area_mm2));
    REQUIRE(a.best.angular_deficit_mm2==Catch::Approx(b.best.angular_deficit_mm2));
    REQUIRE(a.best.gx==Catch::Approx(b.best.gx));REQUIRE(a.best.gy==Catch::Approx(b.best.gy));
}

TEST_CASE("Underside checks label automatic support references and report flat bridges without certifying them", "[curved-blend][undersides]")
{
    const auto r=analyze_undersides({underside(0.)},.08,0.);
    REQUIRE_FALSE(r.explicit_threshold);REQUIRE(r.has_candidate);
    REQUIRE(r.planar.below_threshold_mm2==Catch::Approx(50.));
    REQUIRE(underside_analysis_summary(r).find("automatic support uses bead overlap")!=std::string::npos);
    REQUIRE(underside_analysis_summary(r).find("not support savings")!=std::string::npos);
    REQUIRE(underside_analysis_summary(r).find("support/bridge generation and anchoring checks")!=std::string::npos);
}

TEST_CASE("Underside threshold classification ignores float STL normal noise", "[curved-blend][undersides]")
{
    const auto r=analyze_undersides({underside(29.9999976)},.08,30.);
    REQUIRE(r.facets==1);REQUIRE(r.planar.below_threshold_mm2==0.);
    REQUIRE(r.planar.angular_deficit_mm2==0.);REQUIRE_FALSE(r.has_candidate);
    REQUIRE(analyze_undersides({underside(29.99)},.08,30.).planar.below_threshold_mm2==Catch::Approx(50.));
}

TEST_CASE("Underside checks reject invalid geometry and honor cancellation", "[curved-blend][undersides]")
{
    REQUIRE_THROWS(analyze_undersides({underside(20.)},-.1,30.));
    REQUIRE_THROWS(analyze_undersides({underside(20.)},.1,91.));
    REQUIRE_THROWS(analyze_undersides({underside(20.)},.1,std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_THROWS(analyze_undersides({{{{0.,0.,1.},{0.,0.,1.},{0.,0.,1.}}}},.1,30.));
    auto t=underside(20.);t[0][0]=std::numeric_limits<double>::infinity();
    REQUIRE_THROWS(analyze_undersides({t},.1,30.));
    size_t calls=0;
    REQUIRE_THROWS(analyze_undersides({underside(20.)},.1,30.,[&]{if(++calls==2) throw std::runtime_error("canceled");}));
    REQUIRE(calls==2);
}

TEST_CASE("Underside candidates account for new overhangs on initially vertical walls", "[curved-blend][undersides]")
{
    // A deliberately broad synthetic bound exercises printers unlike the
    // five-degree Centauri fixture. Tilting along -X would turn this large
    // vertical wall into a critical underside. Tilting toward Y avoids it.
    const Ramp::Triangle wall{{{0.,0.,1.},{0.,0.,101.},{0.,100.,1.}}}; // outward -X
    const double allowed=std::tan(70.*3.14159265358979323846/180.);
    const auto r=analyze_undersides({underside(20.),wall},allowed,80.);
    REQUIRE(r.has_candidate);REQUIRE(r.facets==1);
    REQUIRE(std::abs(r.best.gx)<1e-5);REQUIRE(std::abs(r.best.gy)>.9*allowed);
    REQUIRE(r.best.worsened_mm2==0.);
}
