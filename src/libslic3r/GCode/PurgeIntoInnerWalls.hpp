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
// first layer or an object whose entry filament no one tracked) and is left
// at the layer's last filament so the caller carries it into the next object
// or layer. Repeating the same filament is not a toolchange.
//
// Every place that sizes purge capacity - the demand-driven growth of model
// walls and support walls, the proportional reduction at allocation time, and
// the allocation itself - walks a layer this way. Keeping the walk here keeps
// those four estimates consistent with each other.
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
// A matching tier is ordered by color adjacency first - the loop nearest that
// exterior is taken before any volume consideration, because a wall that
// continues the purge color can neither bleed nor look wrong, so distance to
// the surface outranks packing efficiency there. Volume only breaks equal-depth
// ties.
//
// A foreign tier is ordered by ascending volume, which is also the scan order
// pick_volume_fit() needs, with the deepest loop first at equal volume.
inline bool wall_before(bool a_matches_purge_color, int a_inset, bool a_existing, float a_volume,
                        bool b_matches_purge_color, int b_inset, bool b_existing, float b_volume)
{
    if (a_matches_purge_color != b_matches_purge_color)
        return a_matches_purge_color; // color adjacency outranks everything
    if (a_matches_purge_color) {
        const auto a_depth = wall_depth_priority(true, a_inset, a_existing);
        const auto b_depth = wall_depth_priority(true, b_inset, b_existing);
        return a_depth != b_depth ? a_depth > b_depth : a_volume < b_volume;
    }
    return a_volume != b_volume ? a_volume < b_volume :
           wall_depth_priority(false, a_inset, a_existing) > wall_depth_priority(false, b_inset, b_existing);
}

// Which loop to claim next. `count` must be non-zero and the candidates must
// already be in wall_before() order.
//
// A matching tier claims strictly in that order, i.e. nearest its own exterior
// first: adjacency to the surface is the requirement, so it is never traded
// against how well a loop is sized. A foreign tier is best-fit instead - prefer
// the smallest loop that can absorb the whole remainder: a small flush must not
// burn a large wall that the print needs anyway, and one wall that takes
// everything is better than several that each waste part of themselves. When no
// loop can absorb the remainder, take the largest, so packing converges instead
// of stalling on the smallest loops.
template<typename VolumeAt>
inline size_t pick_volume_fit(size_t count, VolumeAt volume_at, float remaining)
{
    size_t largest = count;
    float  largest_volume = 0.f;
    for (size_t i = 0; i < count; ++i) {
        const float volume = volume_at(i);
        if (volume >= remaining)
            return i; // ascending order: the first that absorbs the remainder is the smallest
        if (largest == count || volume > largest_volume) {
            largest = i;
            largest_volume = volume;
        }
    }
    return largest;
}

// Index of the unclaimed slot to take next, over `count` candidates already in
// wall_before() order, or `count` when the tier is exhausted.
template<typename VolumeAt>
inline size_t pick_next_claim(size_t count, bool matches_purge_color, VolumeAt volume_at, float remaining)
{
    if (count == 0)
        return count; // exhausted; a matching tier would otherwise report slot 0
    if (!matches_purge_color)
        return pick_volume_fit(count, volume_at, remaining);
    return 0;
}

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
