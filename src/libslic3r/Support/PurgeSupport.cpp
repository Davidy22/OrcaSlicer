#include "PurgeSupport.hpp"

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../ExtrusionEntity.hpp"
#include "../Polygon.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace Slic3r {

PurgeSupportRegion::PurgeSupportRegion(ExtrusionEntityCollection &paths, ExPolygons areas, const Flow &flow, size_t base_wall_count)
    : m_paths(&paths), m_areas(std::move(areas)), m_flow(flow),
      m_base_wall_count(base_wall_count), m_wall_count(base_wall_count)
{
}

void PurgeSupportRegion::reset()
{
    if (m_original) {
        // Transfer the saved native paths back; cloning would temporarily keep
        // the grown paths and two copies of the original support alive.
        m_paths->swap(*m_original);
        m_original.reset();
    }
    m_wall_count = m_base_wall_count;
}

double PurgeSupportRegion::grow(double additional_volume)
{
    if (additional_volume <= EPSILON || m_areas.empty() || m_flow.spacing() <= EPSILON)
        return 0.;
    const double original_volume = m_paths->total_volume();
    const double target_volume = original_volume + additional_volume;
    double volume = original_volume;
    const double spacing = m_flow.scaled_spacing();
    ExPolygons loop_areas = offset_ex(m_areas, -0.5 * spacing);
    ExtrusionEntityCollection walls;
    size_t wall_count = 0;
    while (!loop_areas.empty() && volume + EPSILON < target_volume) {
        ++wall_count;
        extrusion_entities_append_loops(walls.entities, to_polygons(loop_areas), erSupportMaterial,
                                        m_flow.mm3_per_mm(), m_flow.width(), m_flow.height());
        if (wall_count > m_base_wall_count) {
            // Replace the wall band, rather than superimposing walls on the
            // existing infill. Preserve native fill (including its individual
            // heights/flows) in the interior and outside this growth region.
            const ExPolygons interior = offset_ex(loop_areas, -0.5 * spacing);
            const ExPolygons wall_band = diff_ex(offset_ex(m_areas, SCALED_EPSILON), interior);
            ExtrusionEntityCollection candidate(walls);
            candidate.no_sort = true; // support outlines before remaining fill
            for_each_extrusion_path(m_original ? *m_original : *m_paths, [&](const ExtrusionPath &path) {
                path.subtract_expolygons(wall_band, &candidate);
            });
            const double candidate_volume = candidate.total_volume();
            // Dense first layers or narrow branches may already have as much
            // capacity as these loops. Never reduce their existing capacity.
            if (candidate_volume > volume + EPSILON) {
                // Save the native paths only on accepted growth, transferring
                // ownership instead of deep-copying paths we are about to replace.
                if (!m_original)
                    m_original = std::make_unique<ExtrusionEntityCollection>(std::move(*m_paths));
                volume = candidate_volume;
                m_wall_count = wall_count;
                m_paths->swap(candidate);
            }
        }
        // Same shrink/open operation as native tree support walls: narrow
        // remnants disappear instead of producing overlapping/overflowing loops.
        loop_areas = offset2_ex(loop_areas, -1.4 * spacing, 0.4 * spacing);
    }
    return std::max(0., volume - original_volume);
}

} // namespace Slic3r
