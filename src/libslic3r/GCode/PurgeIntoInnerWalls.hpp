#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <optional>
#include <tuple>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::PurgeIntoInnerWalls {

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

// Larger keys are allocated first: preferred objects, then the largest loop
// volumes. Color, depth and existing capacity only break equal-volume ties;
// native exterior/shield protection is enforced before this ranking.
inline auto wall_priority(bool preferred, bool matches_outer_wall, int inset_idx, bool existing, float volume)
{
    return std::make_tuple(preferred, volume, matches_outer_wall, matches_outer_wall ? -inset_idx : inset_idx, existing);
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
