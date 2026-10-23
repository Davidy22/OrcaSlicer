#pragma once

#include "../ExPolygon.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"

#include <cstddef>
#include <memory>

namespace Slic3r {

// A support generator supplies its actual printable base area and flow. The
// collection remains owned by SupportLayer::support_fills; neither the model
// nor support interfaces/overlapping-height regions are enlarged by this helper.
class PurgeSupportRegion
{
public:
    PurgeSupportRegion(ExtrusionEntityCollection &paths, ExPolygons areas, const Flow &flow, size_t base_wall_count);

    // Restore native paths before replanning, so changing demand never leaves
    // stale extra walls behind. The serialized Support wall loops is unchanged.
    void reset();
    // Add inward loops until this much additional capacity fits, or the area
    // is solid. Returns the actual added volume, not an estimated loop volume.
    double grow(double additional_volume);
    size_t wall_count() const { return m_wall_count; }
    const ExtrusionEntityCollection *collection() const { return m_paths; }

private:
    ExtrusionEntityCollection *m_paths;
    ExPolygons m_areas;
    Flow m_flow;
    size_t m_base_wall_count;
    size_t m_wall_count;
    std::unique_ptr<ExtrusionEntityCollection> m_original;
};

} // namespace Slic3r
