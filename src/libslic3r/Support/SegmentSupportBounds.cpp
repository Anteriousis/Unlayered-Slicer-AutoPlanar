#include "SegmentSupportBounds.hpp"
#include "../ClipperUtils.hpp"
#include "../Print.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

// Norms below this are numerical noise, not a direction. Deliberately far tighter than EPSILON:
// these are unit-ish normals, and a clip only ever degenerates through an authored plane that was
// already rejected upstream.
static constexpr double NORMAL_EPSILON = 1e-12;

SegmentSupportBounds::SegmentSupportBounds(const CutSegmentDescriptor &segment,
                                          const Transform3d          &object_to_slice,
                                          double                      bed_clearance)
{
    // Normals are covectors: they carry through the inverse transpose of the linear part, which
    // stays correct under rotation, non-uniform scale and mirroring. Points carry through the
    // transform itself.
    const Eigen::Matrix3d normal_to_slice = object_to_slice.linear().inverse().transpose();
    for (const CutSegmentClip &clip : segment.clips) {
        const double norm_local_sq = clip.normal.squaredNorm();
        if (norm_local_sq < NORMAL_EPSILON)
            continue;
        // Clip membership is  clip.normal . p - clip.offset <= 0; negating gives the into-segment
        // form this class stores,  n . x >= d.
        Vec3d        n  = normal_to_slice * (- clip.normal);
        const double nn = n.norm();
        if (nn < NORMAL_EPSILON)
            continue;
        n /= nn;
        // Foot of the perpendicular from the object-local origin onto the clip plane -- any point
        // on the plane will do, and this one needs no basis.
        const Vec3d p = object_to_slice * (clip.normal * (clip.offset / norm_local_sq));
        m_planes.push_back({ n, n.dot(p), clip.role == CutClipRole::Ceiling });
    }

    if (segment.is_root()) {
        // The root is the only segment that rests on the build plate; every other one rests on its
        // parent through its base plane. The root slices along +Z exactly (see
        // PrintObject::effective_layer_rotation_bc), so its slice frame differs from the instance
        // frame by translation alone: XY centering, and -build_z_min in Z, which is what puts the
        // root's own minimum at slice z = 0 -- except that a SUNK root is deliberately NOT rebased,
        // so the material under the plate is never sliced (see CutSegmentDescriptor::slice_z_base).
        // A world point's slice z is therefore world_z - slice_z_base(), so the plate (world z = 0)
        // sits at slice z = -slice_z_base(), raised
        // by bed_clearance. An object floating above the plate keeps the plate where it is (a
        // negative slice z) instead of pretending the floor is at the model's lowest material.
        m_planes.push_back({ Vec3d::UnitZ(), bed_clearance - segment.slice_z_base(), false });
    }

    m_active = ! m_planes.empty();
}

SegmentSupportBounds::SegmentSupportBounds(const PrintObject &object, double bed_clearance)
{
    // An ordinary object carries no descriptor: stay inactive so every caller keeps stock behavior.
    if (! object.is_segment())
        return;
    *this = SegmentSupportBounds(*object.segment(), object.object_to_slice(), bed_clearance);
}

Polygons SegmentSupportBounds::valid_region_polygon(coordf_t print_z) const
{
    // Support is allowed inside EVERY bound -> intersection of the per-plane segment-side
    // half-planes. Ceilings participate: they clip, they just never bear load.
    Polygons result;
    bool     first = true;
    for (const Plane &pl : m_planes) {
        Polygons hp = half_plane(pl, print_z, true);
        if (first) {
            result = std::move(hp);
            first  = false;
        } else {
            result = intersection(result, hp);
        }
        if (result.empty())
            break;
    }
    return result;
}

double SegmentSupportBounds::floor_z(const Point &p) const
{
    double       z     = - std::numeric_limits<double>::max();
    bool         found = false;
    const double x     = unscale<double>(p.x());
    const double y     = unscale<double>(p.y());
    for (const Plane &pl : m_planes)
        if (! pl.ceiling && std::abs(pl.n.z()) >= NORMAL_EPSILON) {
            z     = std::max(z, (pl.d - pl.n.x() * x - pl.n.y() * y) / pl.n.z());
            found = true;
        }
    // No floor under this point (only vertical floors, or ceilings alone): report 0.0 so the caller
    // treats it as "no floor to descend to" (deepest >= -EPSILON) rather than -inf, which would
    // drive an unbounded downward extension.
    return found ? z : 0.0;
}

Polygons SegmentSupportBounds::half_plane(const Plane &pl, coordf_t print_z, bool segment_side) const
{
    const double  gx    = pl.n.x();
    const double  gy    = pl.n.y();
    const double  gnorm = std::hypot(gx, gy);
    const coord_t BIG   = scale_(1e4);   // far beyond any bed; bounds the half-plane for Clipper

    if (gnorm < 1e-9) {
        // Plane (nearly) horizontal in the slice frame: the whole layer is on one side of it.
        const bool inside = (pl.n.z() * double(print_z) >= pl.d);
        if (inside == segment_side) {
            Polygon sq;
            sq.points = { Point(-BIG, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(-BIG, BIG) };
            return { sq };
        }
        return {};
    }

    // Boundary line in slice XY: n_x*x + n_y*y = d - n_z*z0. Signed offset along the unit gradient
    // is s; in the frame rotated so the gradient is +X, the segment side is x >= s.
    const double  s_mm = (pl.d - pl.n.z() * double(print_z)) / gnorm;
    const coord_t s    = scale_(s_mm);

    // Degenerate: the split line lies outside the bounded box (e.g. a layer far below the plane),
    // so the whole region is on one side. Avoids an inverted rectangle that Clipper would mishandle.
    if (s >= BIG || s <= -BIG) {
        const bool segment_full = (s <= -BIG);   // entire box satisfies x >= s
        if (segment_full == segment_side) {
            Polygon sq;
            sq.points = { Point(-BIG, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(-BIG, BIG) };
            return { sq };
        }
        return {};
    }

    const double ang = std::atan2(gy, gx);
    Polygon      poly;
    if (segment_side)
        poly.points = { Point(s, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(s, BIG) };
    else
        poly.points = { Point(-BIG, -BIG), Point(s, -BIG), Point(s, BIG), Point(-BIG, BIG) };
    poly.rotate(ang);   // rotate the axis-aligned rectangle back into the slice XY frame
    return { poly };
}

} // namespace Slic3r
