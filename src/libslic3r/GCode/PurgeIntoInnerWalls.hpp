#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <tuple>

namespace Slic3r::PurgeIntoInnerWalls {

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
