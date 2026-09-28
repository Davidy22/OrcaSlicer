#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Slic3r {

// Layer-local geometry input, not a modification of the user's wall_loops.
// planned_volume_mm3 is measured capacity, not material already allocated to a transition.
struct PurgeInnerWallPlan
{
    int    extra_loops = 0;
    double target_volume_mm3 = 0.;
    double planned_volume_mm3 = 0.;
    bool   capacity_limited = false;
};

// Whole extrusion entities are indivisible purge targets. Aggregate volume is
// insufficient when many transitions each consume only part of a large loop.
inline double purge_inner_wall_shortfall(const std::vector<double> &transitions, const std::vector<double> &ordered_capacities)
{
    for (double volume : ordered_capacities)
        if (!std::isfinite(volume) || volume < 0.) throw std::invalid_argument("Invalid purge loop capacity");
    size_t next = 0;
    double missing = 0.;
    for (double volume : transitions) {
        if (!std::isfinite(volume) || volume < 0.) throw std::invalid_argument("Invalid purge transition");
        while (volume > 0. && next < ordered_capacities.size()) volume -= ordered_capacities[next++];
        missing += std::max(0., volume);
        if (!std::isfinite(missing)) throw std::invalid_argument("Purge shortfall overflow");
    }
    return missing;
}

} // namespace Slic3r
