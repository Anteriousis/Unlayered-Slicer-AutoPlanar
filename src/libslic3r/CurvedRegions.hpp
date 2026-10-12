#pragma once
#include "CurvedRamp.hpp"
#include <vector>
#include <limits>

namespace Slic3r::CurvedTransition {

// Disjoint compact C2 fields. Each roof patch has its own direction; the rest
// of the model (including protected underside/contact regions) stays planar.
// w=z-S(z)*g.(x-c)*phi(|x-c|/r), phi=(1-r)^4(1+4r).
class Regions final : public Field {
public:
    struct Patch { double x,y,radius,lo,hi,gx,gy; };
    Regions(ModelDomain domain,std::vector<Patch> patches,double allowed)
        : Field(Options{},{}),m_domain(domain),m_patches(std::move(patches))
    {
        if(m_patches.empty() || !std::isfinite(allowed) || allowed<=0.)
            throw std::invalid_argument("No feasible regional curved surface");
        for(size_t i=0;i<m_patches.size();++i) {
            const auto& p=m_patches[i];
            for(double v:{p.x,p.y,p.radius,p.lo,p.hi,p.gx,p.gy})
                if(!std::isfinite(v)) throw std::invalid_argument("Nonfinite curved region");
            if(p.radius<=0. || p.lo<0. || p.hi<=p.lo)
                throw std::invalid_argument("Invalid curved region bounds");
            for(size_t j=0;j<i;++j) if(std::hypot(p.x-m_patches[j].x,p.y-m_patches[j].y)<p.radius+m_patches[j].radius)
                throw std::invalid_argument("Curved region footprints must not overlap");
            const double g=std::hypot(p.gx,p.gy),depth=p.hi-p.lo;
            // max r*phi(r)<0.17. The eigenvalues of grad(u*phi) are
            // phi in [0,1] and phi+r*phi' in [-7/16,1]. Thus |grad L|<=|g|.
            const double a=.17*g*p.radius,d=1.875*a/depth;
            m_amplitude=std::max(m_amplitude,a);
            m_jmin=std::min(m_jmin,1.-d);m_jmax=std::max(m_jmax,1.+d);
            m_g=std::max(m_g,g);
            m_hessian=std::max(m_hessian,22.*g/p.radius+3.75*g/depth+
                (10.*std::sqrt(3.)/3.)*a/(depth*depth));
        }
        if(m_jmin<.75 || m_g/m_jmin>allowed+1e-10)
            throw std::invalid_argument("Regional blend exceeds monotonicity or slope clearance");
        m_reference=domain.top_max+m_amplitude;
    }
    bool enabled() const override {return true;}
    double reference_top_mm() const override {return m_reference;}
    void set_reference_top(double z) {m_reference=z;}
    double slope_bound() const override {return m_g/m_jmin;}
    double jacobian_min() const override {return m_jmin;}
    double jacobian_max() const override {return m_jmax;}
    double max_displacement_mm() const override {return m_amplitude;}
    double inverse_hessian_bound() const override {return m_hessian;}
    double inverse_hessian_bound_box(double xmin,double xmax,double ymin,double ymax,double zmin,double zmax) const override {
        return identity_on_box(xmin,xmax,ymin,ymax,zmin,zmax)?0.:m_hessian;
    }
    bool identity_on_box(double xmin,double xmax,double ymin,double ymax,double zmin,double zmax) const override {
        (void)zmin;
        for(const auto& p:m_patches) {
            const double dx=std::max({xmin-p.x,0.,p.x-xmax});
            const double dy=std::max({ymin-p.y,0.,p.y-ymax});
            if(zmax>p.lo && dx*dx+dy*dy<p.radius*p.radius) return false;
        }
        return true;
    }
    const std::vector<Patch>& patches() const {return m_patches;}
    double slice_z(double x,double y,double z) const override {
        const auto q=displacement(x,y,z);return z-q.value;
    }
    double world_z(double x,double y,double w) const override {
        check(x,y,w);
        if(identity_on_box(x,x,y,y,w-m_amplitude,w+m_amplitude)) return w;
        double lo=w-m_amplitude,hi=w+m_amplitude;
        for(int i=0;i<48;++i) {const double mid=(lo+hi)*.5;if(slice_z(x,y,mid)<w) lo=mid;else hi=mid;}
        return (lo+hi)*.5;
    }
    Gradient gradient(double x,double y,double w) const override {
        const auto q=displacement(x,y,world_z(x,y,w));
        return {q.dx/(1.-q.dz),q.dy/(1.-q.dz)};
    }
private:
    struct Sample {double value=0.,dx=0.,dy=0.,dz=0.;};
    static void check(double x,double y,double z) {
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))
            throw std::invalid_argument("Nonfinite regional field coordinate");
    }
    Sample displacement(double x,double y,double z) const {
        check(x,y,z);
        for(const auto& p:m_patches) {
            const double ux=x-p.x,uy=y-p.y,d=std::hypot(ux,uy),r=d/p.radius;
            if(r>=1. || z<=p.lo) continue;
            const double a=1.-r,phi=a*a*a*a*(1.+4.*r),L=p.gx*ux+p.gy*uy;
            double s=1.,ds=0.;
            if(z<p.hi) {const double t=(z-p.lo)/(p.hi-p.lo);s=t*t*t*(10.+t*(-15.+6.*t));ds=30.*t*t*(1.-t)*(1.-t)/(p.hi-p.lo);}
            const double radial=d>1e-12?-20.*r*a*a*a/(p.radius*d):0.;
            return {s*L*phi,s*(p.gx*phi+L*radial*ux),s*(p.gy*phi+L*radial*uy),ds*L*phi};
        }
        return {};
    }
    ModelDomain m_domain;
    std::vector<Patch> m_patches;
    double m_reference=0.,m_amplitude=0.,m_jmin=1.,m_jmax=1.,m_g=0.,m_hessian=0.;
};

// Roof selection is a heuristic. Accepted fields have analytic bounds above;
// bridge/support toolpaths are checked against their full bead boxes at export.
inline std::vector<Regions::Patch> select_regions(const std::vector<Ramp::Triangle>& mesh,
    double first,double step,double allowed,double solid_depth,bool sparse)
{
    struct Face {double x,y,z,gx,gy,area;};
    struct Box {double xmin,xmax,ymin,ymax,zmax;};
    std::vector<Face> roofs;std::vector<Box> contacts;
    for(const auto& t:mesh) {
        const auto& a=t[0];const auto& b=t[1];const auto& c=t[2];
        const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        if(!std::isfinite(nx+ny+nz) || std::hypot(std::hypot(nx,ny),nz)<=1e-12)
            throw std::invalid_argument("Invalid regional mesh facet");
        const double zmax=std::max({a[2],b[2],c[2]});
        if(nz<0. && zmax>2e-5) contacts.push_back({std::min({a[0],b[0],c[0]})-1.,std::max({a[0],b[0],c[0]})+1.,
            std::min({a[1],b[1],c[1]})-1.,std::max({a[1],b[1],c[1]})+1.,zmax});
        if(nz<=0.) continue;
        const double gx=-nx/nz,gy=-ny/nz,g=std::hypot(gx,gy);
        if(g<std::tan(.5*3.14159265358979323846/180.) || g>std::tan(30.*3.14159265358979323846/180.)) continue;
        roofs.push_back({(a[0]+b[0]+c[0])/3.,(a[1]+b[1]+c[1])/3.,(a[2]+b[2]+c[2])/3.,gx,gy,nz*.5});
    }
    std::stable_sort(roofs.begin(),roofs.end(),[](const Face& a,const Face& b){return a.area>b.area;});
    std::vector<Regions::Patch> patches;
    for(const auto& f:roofs) {
        double radius=std::clamp(std::sqrt(f.area)*.7,1.,6.);
        bool overlap=false;
        for(const auto& p:patches) if(std::hypot(f.x-p.x,f.y-p.y)<radius+p.radius) {overlap=true;break;}
        if(overlap) continue;
        const double g=std::hypot(f.gx,f.gy),scale=std::min(1.,.70*allowed/g);
        const double gx=f.gx*scale,gy=f.gy*scale;
        if(std::hypot(gx,gy)<std::tan(.5*3.14159265358979323846/180.) || scale<.10) continue;
        const double amplitude=.17*std::hypot(gx,gy)*radius;
        const double depth=std::max(sparse?step:3.*step,1.875*amplitude/.20);
        // Leave the first top-shell bridge horizontal in sparse jobs. The
        // field starts above its bead and keeps all protected undersides flat.
        double lo=first;
        if(sparse) lo=std::max(lo,f.z-solid_depth+step);
        for(const auto& b:contacts) if(f.x+radius>b.xmin && f.x-radius<b.xmax && f.y+radius>b.ymin && f.y-radius<b.ymax)
            lo=std::max(lo,b.zmax+3.*step);
        if(lo+depth+step*.5>f.z) continue;
        patches.push_back({f.x,f.y,radius,lo,lo+depth,gx,gy});
        if(patches.size()==64) break;
    }
    return patches;
}
} // namespace Slic3r::CurvedTransition
