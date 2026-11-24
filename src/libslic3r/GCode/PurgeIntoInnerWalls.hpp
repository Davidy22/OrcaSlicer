#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::PurgeIntoInnerWalls {

// Grown purge walls taper out downward: the layer below a wall spike carries
// one ring fewer for every layer in between, so the extra capacity fades out
// instead of running the full height of the object or of its supports. Both
// the model wall growth (PrintObject::make_perimeters()) and the support wall
// growth (Print::_plan_purge_into_inner_walls()) apply exactly this rule; it
// lives here so the two paths cannot drift apart.
template<typename T>
inline T tapered_rings(T rings, T layers_between)
{
    return rings > layers_between ? rings - layers_between : T(0);
}

// Visits every toolchange of a layer's filament sequence in print order.
// `previous` is the filament the layer starts with (-1 when unknown, for the
// first layer or an object whose entry filament no one tracked) and is left at
// the layer's last filament, so the caller carries it into the next object or
// layer. Repeating the same filament is not a toolchange. Every purge estimate
// (model demand, support deficit, proportional reduction, allocation) walks a
// layer this way, which keeps those volumes comparable.
template<typename Extruders, typename F>
inline void for_each_toolchange(const Extruders &extruders, int &previous, F &&fn)
{
    for (const unsigned int filament : extruders) {
        if (previous >= 0 && filament != (unsigned int) previous)
            fn((unsigned int) previous, filament);
        previous = (int) filament;
    }
}

// Flush volume of the toolchanges a layer can purge, in print order.
// `volume_at(from, to)` supplies the directional estimate and returns 0 for a
// pair the configuration forbids purging between. The accumulator type is
// explicit at the call site so each estimator keeps summing in the precision
// it used before.
template<typename Volume, typename Extruders, typename VolumeAt>
inline Volume purge_demand(const Extruders &extruders, int &previous, VolumeAt &&volume_at)
{
    Volume demand = Volume(0);
    for_each_toolchange(extruders, previous, [&demand, &volume_at](unsigned int from, unsigned int to) {
        demand += volume_at(from, to);
    });
    return demand;
}

// Sample the closed loop by arc length, not vertex index: a long straight
// wall must be as likely a target as the same wall split into many segments.
// Prefer the masked edges if any have length; otherwise use the whole loop.
inline std::optional<Point> seam_point(const Points &polygon, double fraction,
                                       const std::vector<bool> &preferred_edges = {})
{
    if (polygon.size() < 2 || !std::isfinite(fraction))
        return std::nullopt;
    if (!preferred_edges.empty() && preferred_edges.size() != polygon.size())
        return std::nullopt;
    double perimeter = 0., preferred_perimeter = 0.;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const double length = (polygon[(i + 1) % polygon.size()].cast<double>() - polygon[i].cast<double>()).norm();
        perimeter += length;
        if (!preferred_edges.empty() && preferred_edges[i])
            preferred_perimeter += length;
    }
    if (perimeter <= 0.)
        return std::nullopt;

    const bool use_preferred = preferred_perimeter > 0.;
    double distance = std::clamp(fraction, 0., 1.) * (use_preferred ? preferred_perimeter : perimeter);
    Point last_point = polygon.front();
    for (size_t i = 0; i < polygon.size(); ++i) {
        if (use_preferred && !preferred_edges[i])
            continue;
        const Vec2d edge = polygon[(i + 1) % polygon.size()].cast<double>() - polygon[i].cast<double>();
        const double length = edge.norm();
        if (length <= 0.)
            continue;
        last_point = polygon[(i + 1) % polygon.size()];
        if (distance <= length)
            return Point((polygon[i].cast<double>() + edge * (distance / length)).cast<coord_t>());
        distance -= length;
    }
    return last_point; // accumulated rounding at fraction 1
}

// Tiers are consumed in this order: preferred objects first, then walls that
// can carry the purge color without repainting anything. A color match is not
// merely a tie-breaker: purge placed in a wall (or a region) that already
// prints that color cannot discolor anything, so those walls are exhausted
// before a foreign region gives up its native walls. Within a tier, color
// adjacency still wins for matching walls (see wall_before). Native
// exterior/shield protection is enforced before any of this.
inline auto wall_tier(bool preferred, bool matches_purge_color)
{
    return std::make_tuple(preferred, matches_purge_color);
}

// Depth preference: a matching wall is taken nearest its own exterior (the
// purge color continues through it), a foreign wall deepest first, so the loop
// beside the visible boundary keeps its native color. Reusing an existing wall
// before a purge-generated one saves the extra loop. Larger keys are preferred.
inline auto wall_depth_priority(bool matches_purge_color, int inset_idx, bool existing)
{
    return std::make_tuple(matches_purge_color ? -inset_idx : inset_idx, existing);
}

// Claim order inside one tier: true when `a` comes before `b`.
//
// Both tiers are claimed strictly in this order, front to back, and the
// allocator never re-picks a loop out of sequence. That is the point: the purge
// color has to end up as one contiguous run. A pick that jumps back out to a
// larger loop leaves a foreign loop sandwiched inside the purged band, which is
// a color seam in the middle of the wall stack - visible as banding where the
// interior of the part should hide the transition.
//
// A matching tier is ordered by color adjacency first - the loop nearest that
// exterior is taken before any volume consideration, because a wall that
// continues the purge color can neither bleed nor look wrong, so distance to
// the surface outranks packing efficiency there. Volume only breaks equal-depth
// ties.
//
// A foreign tier is ordered by depth first, deepest loop first, and volume only
// breaks equal-depth ties. Depth has to outrank volume because volume is only a
// proxy for depth inside a single island: a small object's boundary loop can be
// the smallest loop on the layer while a large object's deepest loop is much
// bigger, so volume alone sends the purge back out to the visible wall of
// whichever object has the tiniest loops. Depth is the property that actually
// hides it, so the loop furthest from any visible surface is claimed first and
// the band grows outwards from the centre. Ties also prefer reusing an existing
// wall over a purge-generated one (see wall_depth_priority).
inline bool wall_before(bool a_matches_purge_color, int a_inset, bool a_existing, float a_volume,
                        bool b_matches_purge_color, int b_inset, bool b_existing, float b_volume)
{
    if (a_matches_purge_color != b_matches_purge_color)
        return a_matches_purge_color; // color adjacency outranks everything
    // Both tiers compare the same way; wall_depth_priority() holds the only
    // difference between them, namely which end of the stack counts as near.
    const auto a_depth = wall_depth_priority(a_matches_purge_color, a_inset, a_existing);
    const auto b_depth = wall_depth_priority(a_matches_purge_color, b_inset, b_existing);
    return a_depth != b_depth ? a_depth > b_depth : a_volume < b_volume;
}

// Claimed front to back: the allocator consumes the tier in wall_before()
// order and drops each claimed loop, see ToolOrdering::mark_wiping_extrusions.
// No loop is ever picked out of turn, so a flush covers a prefix of that order
// and the purge color stays one contiguous run instead of hopping between the
// loops that happen to fit its remainder.

// WipeTower2's purge ramp, in mm/s rather than its writer's mm/min. Advance once
// per purged wall, not once per path segment (Arachne/overhang/scarf can split a
// wall). Neither the ramp nor the flush-flow limit may accelerate a slower path.
inline double limit_speed(double speed, double target_speed, size_t wall_index,
                          double flush_volumetric_speed, double mm3_per_mm)
{
    constexpr std::array<double, 4> ramp{0.33, 0.375, 0.458, 0.875};
    const double ramp_speed = wall_index < ramp.size()
        ? target_speed * ramp[wall_index]
        : std::min(target_speed, 0.875 * target_speed + double(wall_index - 3) * (50. / 60.));
    speed = std::min(speed, ramp_speed);
    // Zero (or a nullable, unset filament override) keeps the normal filament
    // volumetric limit, which GCode::_extrude has already applied.
    if (flush_volumetric_speed > 0. && mm3_per_mm > 0.)
        speed = std::min(speed, flush_volumetric_speed / mm3_per_mm);
    return speed;
}

} // namespace Slic3r::PurgeIntoInnerWalls
