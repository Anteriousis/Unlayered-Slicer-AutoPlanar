#pragma once

// Declarative cut planes: the support-generation view of one segment's bounding half-spaces.
//
// A segment is confined by the clips on its CutSegmentDescriptor (object-local half-spaces,
// `normal.dot(p) - offset <= 0`). This class re-expresses those clips in the segment's own SLICE
// frame, where support generation works, and answers the two questions the generators ask:
//
//   * where support is allowed at a given layer   -> valid_region_polygon()
//   * how far down a branch may descend at an XY  -> floor_z()
//
// Roles matter. A far-side clip (CutClipRole::Floor) is load-bearing: the segment rests on that cut
// face, so it defines the floor and is a legitimate surface for branches to stem from. A near-side
// clip (CutClipRole::Ceiling) is a plane the segment sits BELOW -- the cut face of a segment printed
// LATER. Support must stay below it but must never rest on it, which is why ceilings clip the valid
// region yet are excluded from floor_z(). A downward-V segment genuinely rests on two cut faces, so
// every far-side clip is a floor, not just the base plane.
//
// The root segment additionally rests on the build plate; it is the only segment that does, since
// every other segment rests on its parent through its base plane.
//
// This is the declarative-segment replacement for the old world-frame CutPlaneFloor: the clips are
// already exact and object-local, so there is no config round-trip through world coordinates and no
// dependency on cut_group_id / cut_floor / support_ceilings.

#include "../libslic3r.h"
#include "../CutPlaneSegmentation.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"

#include <vector>

namespace Slic3r {

class PrintObject;

class SegmentSupportBounds
{
public:
    SegmentSupportBounds() = default;

    // Build from a descriptor and the frame that maps object-local coordinates to the coordinates
    // the segment is actually sliced in (PrintObject::object_to_slice()). bed_clearance raises the
    // build-plate floor by that many mm so support rests a first-layer height ABOVE the bed rather
    // than exactly on it, keeping the un-rotated G-code clear of the machine's minimum Z. Only the
    // bed plane is raised; cut planes (resting on the parent segment) are unaffected.
    SegmentSupportBounds(const CutSegmentDescriptor &segment,
                         const Transform3d          &object_to_slice,
                         double                      bed_clearance = 0.);

    // Same, for a PrintObject that is one segment of a cut-plane partition. Inactive -- and so a
    // no-op for every caller -- for an ordinary object, which carries no descriptor.
    explicit SegmentSupportBounds(const PrintObject &object, double bed_clearance = 0.);

    bool is_active() const { return m_active; }

    // XY region at layer print_z where support is ALLOWED: the segment side of ALL bounds
    // (intersection of the half-planes), floors and ceilings alike.
    Polygons valid_region_polygon(coordf_t print_z) const;

    // Floor height (slice-frame, unscaled mm) at a scaled XY point: the highest floor there, since
    // a segment rests on whichever of its cut faces is uppermost. Returns 0 when no floor covers
    // the point, so callers read "no floor to descend to" rather than an unbounded descent.
    double floor_z(const Point &p) const;

private:
    // n . x = d, n pointing INTO the segment; a point is inside the bound iff n.x >= d. `ceiling`
    // marks a bound the segment sits below, which clips but never bears load (see the class note).
    struct Plane { Vec3d n; double d; bool ceiling = false; };

    // A large half-plane polygon for the segment side (segment_side = true) or the outside
    // (false) of one bound, at layer print_z. Both directions are kept: valid_region_polygon()
    // needs the inside, and half_plane's own degenerate handling is written in terms of both.
    Polygons half_plane(const Plane &pl, coordf_t print_z, bool segment_side) const;

    bool               m_active = false;
    std::vector<Plane> m_planes;
};

} // namespace Slic3r
