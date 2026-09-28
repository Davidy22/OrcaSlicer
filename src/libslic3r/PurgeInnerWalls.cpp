#include "Print.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "I18N.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace Slic3r {
namespace {

template<class Visitor>
void visit_perimeter_loops(ExtrusionEntityCollection &collection, const Visitor &visit)
{
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *nested = dynamic_cast<ExtrusionEntityCollection *>(entity)) visit_perimeter_loops(*nested, visit);
        else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity)) visit(*loop);
    }
}

Polylines added_wall_centerlines(LayerRegion &region)
{
    Polylines lines;
    visit_perimeter_loops(region.perimeters, [&](const ExtrusionLoop &loop) {
        if (loop.generated_for_purge) loop.collect_polylines(lines);
    });
    return lines;
}

bool usable_inner_foundation(const ExtrusionLoop &loop)
{
    // Never rely on an exterior treatment, a deferred/bridging wall, or a path
    // that has already moved away from this layer's plane.
    return loop.inset_idx > 0 && !loop.print_after_infill && !loop.paths.empty() &&
        std::all_of(loop.paths.begin(), loop.paths.end(), [](const ExtrusionPath &path) {
            return path.role() == erPerimeter && !path.z_contoured;
        });
}

// Visibility masks depend on model slices, not trial wall counts. Compute them
// once, so overflow retries do not repeat every roof intersection for every loop.
ExPolygons hidden_wall_coverage(const LayerRegion &region)
{
    const Layer &layer = *region.layer();
    const auto &config = region.region().config();
    const auto &print_config = layer.object()->print()->config();
    const unsigned int filament = unsigned(std::max(0, config.inner_wall_filament_id.value - 1));
    if (print_config.filament_soluble.get_at(filament) || print_config.filament_is_support.get_at(filament) ||
        !layer.lower_layer || !layer.upper_layer || layer.id() < size_t(std::max(1, config.bottom_shell_layers.value))) return {};
    ExPolygons covered = layer.lower_layer->lslices;
    const int roof_layers = std::max(1, config.top_shell_layers.value);
    const double roof_height = std::max(layer.height, config.top_shell_thickness.value);
    const Layer *above = layer.upper_layer;
    double covered_z = layer.print_z;
    for (int n = 0; n < roof_layers || covered_z < layer.print_z + roof_height - EPSILON; ++n) {
        if (!above) return {};
        covered = intersection_ex(covered, above->lslices);
        if (covered.empty()) return {};
        covered_z = above->print_z;
        above = above->upper_layer;
    }
    return covered;
}

bool hidden_wall(const ExtrusionLoop &loop, const ExPolygons &covered)
{
    return loop.inset_idx >= 2 && !covered.empty() && diff(loop.polygons_covered_by_width(), to_polygons(covered)).empty();
}

double planning_flow_factor(const LayerRegion &region)
{
    const auto &config = region.region().config();
    const auto &object_config = region.layer()->object()->config();
    const auto &flows = region.layer()->object()->print()->config().filament_flow_ratio.values;
    double min_filament_flow = 1.;
    for (double flow : flows)
        if (std::isfinite(flow)) min_filament_flow = std::min(min_filament_flow, std::max(0., flow));
    return config.print_flow_ratio * min_filament_flow *
        (object_config.set_other_flow_ratios ? config.inner_wall_flow_ratio.value : 1.);
}

double hidden_capacity(const LayerRegion &region, const ExPolygons &covered)
{
    double volume = 0.;
    for (const auto *loop : region.perimeters.purge_inner_wall_candidates())
        if (hidden_wall(*loop, covered)) volume += loop->total_volume();
    return volume * planning_flow_factor(region);
}

int loop_limit(const LayerRegion &region)
{
    const int cap = region.layer()->object()->config().flush_inner_walls_max_extra_loops;
    const BoundingBox bounds = get_extents(to_expolygons(region.slices.surfaces));
    if (!bounds.defined) return 0;
    // No inset survives beyond half the smaller bounding-box dimension. The
    // margin accounts for variable-width Arachne beads and the external spacing.
    const double spacing = std::max(1., double(region.flow(frPerimeter).scaled_spacing()));
    const int solid = std::max(0, int(double(bounds.size().minCoeff()) / spacing / 2.) + 3);
    return cap > 0 ? std::min(cap, solid) : solid;
}

void generate_region(LayerRegion &region, int count)
{
    region.purge_inner_wall_plan.extra_loops = count;
    // Rebuild the entire layer: the normal pass may have merged this region
    // with compatible neighbours. Rebuilding just its own slices would leave
    // the old merged island behind and overlap the new walls with that island.
    region.layer()->make_perimeters();
}

// Split verified loops into independent override units. Keeping an island as an
// override unit would also change its external walls' material.
void isolate_purge_loops(LayerRegion &region, const ExPolygons &covered)
{
    ExtrusionEntityCollection result;
    result.no_sort = region.perimeters.no_sort;
    if (!region.perimeters.can_reverse()) result.set_reverse();
    std::vector<const ExtrusionLoop *> safe;
    for (const auto *loop : region.perimeters.purge_inner_wall_candidates())
        if (hidden_wall(*loop, covered)) safe.push_back(loop);
    if (safe.empty()) return;
    result.purge_geometry_locked = region.perimeters.purge_geometry_locked;
    std::stable_sort(safe.begin(), safe.end(), [](const auto *a, const auto *b) { return a->inset_idx > b->inset_idx; });
    for (const auto *loop : safe) {
        ExtrusionLoop copy(*loop);
        copy.purge_safe = true;
        ExtrusionEntityCollection single;
        single.purge_geometry_locked = true;
        single.append(std::move(copy));
        result.append(std::move(single));
    }
    const auto copy_normal = [&](const auto &self, const ExtrusionEntityCollection &source, ExtrusionEntityCollection &target) -> void {
        target.no_sort = source.no_sort;
        target.purge_geometry_locked = source.purge_geometry_locked;
        if (!source.can_reverse()) target.set_reverse();
        for (const auto *entity : source.entities) {
            if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                if (std::find(safe.begin(), safe.end(), loop) == safe.end()) target.append(*loop);
            } else if (const auto *nested = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
                ExtrusionEntityCollection child;
                self(self, *nested, child);
                if (!child.empty()) target.append(std::move(child));
            } else target.append(*entity);
        }
    };
    for (const auto *island : region.perimeters.entities) {
        const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(island);
        if (!collection) {
            const auto *loop = dynamic_cast<const ExtrusionLoop *>(island);
            if (!loop || std::find(safe.begin(), safe.end(), loop) == safe.end()) result.append(*island);
            continue;
        }
        ExtrusionEntityCollection normal;
        copy_normal(copy_normal, *collection, normal);
        if (!normal.empty()) result.append(std::move(normal));
    }
    region.perimeters.clear();
    region.perimeters = std::move(result);
}

} // namespace

bool Print::flush_into_inner_walls() const
{
    return std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObject *object) { return object->config().flush_into_inner_walls.value; });
}

float Print::purge_volume_for_transition(unsigned int old_filament, unsigned int new_filament) const
{
    if (old_filament == new_filament || old_filament == unsigned(-1)) return 0.f;
    const size_t n = m_config.filament_colour.size();
    if (old_filament >= n || new_filament >= n) return 0.f;
    const auto matrix = get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.size());
    const size_t index = size_t(old_filament) * n + new_filament;
    if (index >= matrix.size()) throw SlicingError("Invalid flush-volume matrix for inner-wall purging.");
    const double multiplier = has_wipe_tower() && wipe_tower_type() != WipeTowerType::Type2 && m_config.prime_volume_mode == PrimeVolumeMode::pvmFast ?
        m_config.flush_multiplier_fast.get_at(0) : m_config.flush_multiplier.get_at(0);
    const double volume = matrix[index] * multiplier;
    if (!std::isfinite(volume) || volume < 0.) throw SlicingError("Invalid inner-wall purge volume.");
    return float(volume);
}

void Print::prepare_inner_wall_purge()
{
    {
        std::scoped_lock lock(m_inner_wall_purge_mutex);
        m_inner_wall_purge_transitions.clear();
    }
    if (!flush_into_inner_walls()) return;
    if (m_config.spiral_mode || m_config.nozzle_diameter.size() != 1 || m_config.enable_mixed_color_sublayer)
        throw SlicingError(I18N::translate(L("Flush into inner walls requires a single physical nozzle, normal layers, and mixed-color sublayers disabled.")));

    set_status(60, L("Planning purge into inner walls"));
    std::vector<PrintObject *> objects;
    for (PrintObject *object : m_objects)
        if (object->config().flush_into_inner_walls) {
            objects.push_back(object);
        }
    std::stable_sort(objects.begin(), objects.end(), [](const PrintObject *a, const PrintObject *b) {
        if (a->config().flush_into_objects != b->config().flush_into_objects) return a->config().flush_into_objects.getBool();
        return a->id() < b->id();
    });

    auto restart_dependents = [&](PrintObject &object) {
        std::scoped_lock lock(state_mutex());
        object.invalidate_steps_for_recompute({posPrepareInfill, posInfill, posIroning, posContouring, posSupportMaterial,
            posDetectOverhangsForLift, posEstimateCurledExtrusions, posSimplifyPath, posSimplifyInfill, posSimplifySupportPath});
    };
    // Tool ordering must see normal geometry on a re-plan too, not last run's
    // leftover overrides or a fill surface trimmed by last run's extra walls.
    for (PrintObject *object : objects) {
        bool had_plan = false;
        if (object->m_typed_slices) {
            for (Layer *layer : object->layers()) layer->restore_untyped_slices();
            object->m_typed_slices = false;
        }
        for (Layer *layer : object->layers())
            for (LayerRegion *region : layer->regions()) {
                had_plan |= region->purge_inner_wall_plan.extra_loops > 0;
                region->purge_inner_wall_plan = {};
            }
        if (had_plan) {
            for (Layer *layer : object->layers()) { throw_if_canceled(); layer->make_perimeters(); }
            restart_dependents(*object);
            object->infill();
            object->generate_support_material();
            if (object->m_typed_slices) {
                for (Layer *layer : object->layers()) layer->restore_untyped_slices();
                object->m_typed_slices = false;
            }
        }
    }

    std::map<const LayerRegion *, ExPolygons> coverage;
    for (PrintObject *object : objects)
        for (Layer *layer : object->layers()) {
            throw_if_canceled();
            for (LayerRegion *region : layer->regions()) coverage.emplace(region, hidden_wall_coverage(*region));
        }

    // Use real tool orderings and the same matrix/multiplier as the tool change,
    // after normal perimeters, fills and supports exist. Dedicated objects first.
    using DemandKey = std::pair<const PrintObject *, double>;
    std::map<DemandKey, double> demand;
    std::map<DemandKey, std::vector<double>> transitions;
    auto collect_demand = [&](ToolOrdering &ordering, const PrintObject *only_object) {
        unsigned int previous = ordering.first_extruder();
        if (has_wipe_tower() && wipe_tower_type() == WipeTowerType::Type2 && !ordering.all_extruders().empty())
            previous = ordering.all_extruders().back();
        for (LayerTools &layer : ordering.layer_tools()) {
            for (unsigned int next : layer.extruders) {
                const bool safe_materials = previous != unsigned(-1) && !m_config.filament_soluble.get_at(previous) &&
                    !m_config.filament_soluble.get_at(next) && !m_config.filament_is_support.get_at(previous) && !m_config.filament_is_support.get_at(next);
                const float requested = safe_materials ? purge_volume_for_transition(previous, next) : 0.f;
                const float left = layer.wiping_extrusions().mark_dedicated_purge(*this, previous, next, requested, only_object);
                demand[{only_object, layer.print_z}] += left;
                if (left > EPSILON) transitions[{only_object, layer.print_z}].push_back(left);
                previous = next;
            }
        }
    };
    if (m_config.print_sequence == PrintSequence::ByObject) {
        for (PrintObject *object : m_objects) {
            ToolOrdering ordering(*object, unsigned(-1));
            ordering.sort_and_build_data(*object, unsigned(-1));
            collect_demand(ordering, object);
        }
    } else {
        ToolOrdering ordering(*this, unsigned(-1), false);
        ordering.sort_and_build_data(*this, unsigned(-1), false);
        collect_demand(ordering, nullptr);
    }

    for (const auto &[key, requested] : demand) {
        double remaining = requested;
        for (PrintObject *object : objects) {
            if (key.first && object != key.first) continue;
            Layer *layer = object->get_layer_at_printz(key.second, EPSILON);
            if (!layer) continue;
            const size_t copies = key.first ? 1 : object->instances().size();
            if (copies == 0) continue;
            for (LayerRegion *region : layer->regions()) {
                if (remaining <= EPSILON || coverage.at(region).empty() || region->slices.empty() || region->region().config().wall_loops < 1) continue;
                const int limit = loop_limit(*region);
                int chosen = 0;
                double capacity = 0.;
                for (int count = 1; count <= limit; ++count) {
                    throw_if_canceled();
                    generate_region(*region, count);
                    const double measured = hidden_capacity(*region, coverage.at(region)) * copies;
                    if (measured > capacity + EPSILON) { capacity = measured; chosen = count; }
                    if (capacity >= remaining) break;
                }
                generate_region(*region, chosen);
                region->purge_inner_wall_plan.target_volume_mm3 = remaining / copies;
                remaining = std::max(0., remaining - capacity);
            }
        }
    }

    auto support_stack = [&](PrintObject *object) {
        auto &layers = object->layers();
        // Retain the same number below, a stricter baseline than X-1. A count
        // alone is not a support proof; the bottom-up coverage pass below is.
        for (size_t i = layers.size(); i > 1; --i)
            for (const LayerRegion *upper : layers[i - 1]->regions()) {
                const int upper_count = upper->purge_inner_wall_plan.extra_loops;
                if (upper_count == 0) continue;
                for (LayerRegion *below : layers[i - 2]->regions()) {
                    if (below->slices.empty() || intersection_ex(to_polygons(upper->slices.surfaces), to_polygons(below->slices.surfaces)).empty()) continue;
                    const auto baseline = [](const LayerRegion &r) {
                        const auto &c = r.region().config();
                        return c.wall_loops.value + int(c.alternate_extra_wall && r.layer()->id() % 2 == 1 && c.sparse_infill_density > 0);
                    };
                    const int required = std::min(std::max(upper_count, upper_count + baseline(*upper) - baseline(*below)), loop_limit(*below));
                    if (required > below->purge_inner_wall_plan.extra_loops) generate_region(*below, required);
                }
            }
        for (Layer *layer : layers) {
            throw_if_canceled();
            Polygons support;
            if (layer->lower_layer) {
                for (LayerRegion *below : layer->lower_layer->regions())
                    visit_perimeter_loops(below->perimeters, [&](const ExtrusionLoop &loop) {
                        if (usable_inner_foundation(loop)) loop.polygons_covered_by_width(support, float(SCALED_EPSILON));
                    });
                support = union_(support);
            }
            for (LayerRegion *region : layer->regions()) {
                int count = region->purge_inner_wall_plan.extra_loops;
                if (layer->lower_layer) {
                    int below_count = 0;
                    for (const LayerRegion *below : layer->lower_layer->regions())
                        if (!intersection_ex(to_polygons(region->slices.surfaces), to_polygons(below->slices.surfaces)).empty())
                            below_count = std::max(below_count, below->purge_inner_wall_plan.extra_loops);
                    if (count > below_count + 1) generate_region(*region, count = below_count + 1);
                }
                // Test actual extrusion centerlines, not the solid slice. No
                // sparse infill, same-layer bridge, or future wall is support.
                while (count > 0 && ((layer->lower_layer && !diff_pl(added_wall_centerlines(*region), support).empty()) ||
                                     (!layer->lower_layer && object->config().raft_layers > 0))) {
                    throw_if_canceled();
                    generate_region(*region, --count);
                }
                auto &plan = region->purge_inner_wall_plan;
                plan.planned_volume_mm3 = hidden_capacity(*region, coverage.at(region));
                plan.capacity_limited = plan.planned_volume_mm3 + EPSILON < plan.target_volume_mm3;
            }
        }
    };
    for (PrintObject *object : objects) support_stack(object);

    // Match the allocator's object/copy/inset order, retaining region ownership
    // so demand-free trial plans can be discarded afterwards.
    auto capacities_for = [&](const DemandKey &key) {
        std::vector<std::pair<LayerRegion *, double>> capacities;
        for (PrintObject *object : objects) {
            if (key.first && key.first != object) continue;
            Layer *layer = object->get_layer_at_printz(key.second, EPSILON);
            if (!layer) continue;
            std::vector<std::tuple<int, LayerRegion *, double>> loops;
            for (LayerRegion *region : layer->regions())
                for (const auto *loop : region->perimeters.purge_inner_wall_candidates())
                    if (hidden_wall(*loop, coverage.at(region)))
                        loops.emplace_back(loop->inset_idx, region, loop->total_volume() * planning_flow_factor(*region));
            std::stable_sort(loops.begin(), loops.end(), [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
            for (size_t copy = 0; copy < (key.first ? 1 : object->instances().size()); ++copy)
                for (const auto &loop : loops) capacities.emplace_back(std::get<1>(loop), std::get<2>(loop));
        }
        return capacities;
    };
    std::map<DemandKey, std::pair<std::vector<int>, double>> missing_cache;
    auto missing_for = [&](const DemandKey &key) {
        // Slice geometry is fixed; local counts uniquely determine regenerated
        // paths. Recompute only layers affected by a trial or its support stack.
        std::vector<int> counts;
        for (PrintObject *object : objects)
            if (!key.first || key.first == object)
                if (const Layer *layer = object->get_layer_at_printz(key.second, EPSILON))
                    for (const LayerRegion *region : layer->regions()) counts.push_back(region->purge_inner_wall_plan.extra_loops);
        auto cached = missing_cache.find(key);
        if (cached != missing_cache.end() && cached->second.first == counts) return cached->second.second;
        std::vector<double> capacities;
        for (const auto &entry : capacities_for(key)) capacities.push_back(entry.second);
        const double missing = purge_inner_wall_shortfall(transitions[key], capacities);
        missing_cache[key] = {std::move(counts), missing};
        return missing;
    };
    auto total_missing = [&]() {
        double missing = 0.;
        for (const auto &[key, requested] : demand) missing += missing_for(key);
        return missing;
    };

    // A loop cannot be shared by two transitions. Verify whole-loop budgets,
    // and exhaust other regions before reducing purge. A second bounded pass
    // revisits lower layers whose geometry changed during support propagation.
    for (int verification_pass = 0; verification_pass < 2; ++verification_pass) {
        bool improved = false;
        for (const auto &[key, requested] : demand) {
            auto shortfall = [&]() { return missing_for(key); };
            if (shortfall() <= EPSILON) continue;
            for (PrintObject *object : objects) {
                if (key.first && key.first != object) continue;
                Layer *layer = object->get_layer_at_printz(key.second, EPSILON);
                if (!layer) continue;
                for (LayerRegion *region : layer->regions()) {
                    if (coverage.at(region).empty()) continue;
                    const int limit = loop_limit(*region);
                    for (int count = region->purge_inner_wall_plan.extra_loops + 1; count <= limit && shortfall() > EPSILON; ++count) {
                        throw_if_canceled();
                        const double before = total_missing();
                        std::vector<std::pair<LayerRegion *, PurgeInnerWallPlan>> previous;
                        for (Layer *trial_layer : object->layers())
                            for (LayerRegion *trial_region : trial_layer->regions())
                                previous.emplace_back(trial_region, trial_region->purge_inner_wall_plan);
                        region->purge_inner_wall_plan.target_volume_mm3 = requested / (key.first ? 1 : std::max(size_t(1), object->instances().size()));
                        generate_region(*region, count);
                        if (hidden_capacity(*region, coverage.at(region)) > EPSILON) support_stack(object);
                        if (total_missing() >= before - EPSILON) {
                            // A failed geometry trial can modify the entire support
                            // stack. Restore every affected plan, not just this one
                            // region, and never retain loops that added no capacity.
                            std::set<Layer *> rebuild;
                            for (const auto &[trial_region, old_plan] : previous) {
                                if (trial_region->purge_inner_wall_plan.extra_loops != old_plan.extra_loops)
                                    rebuild.insert(trial_region->layer());
                                trial_region->purge_inner_wall_plan = old_plan;
                            }
                            for (Layer *trial_layer : rebuild) trial_layer->make_perimeters();
                            // Ordinary fuzzy inner walls may get a different
                            // realization on regeneration. Recheck the restored
                            // stack rather than trusting its previous counts.
                            support_stack(object);
                        } else improved = true;
                    }
                }
            }
        }
        if (!improved) break;
    }

    // Retain only regions selected by whole-loop allocation, plus their support
    // dependencies. Intermediate insets within a retained region remain part of
    // its continuous wall stack, not independent purge targets to delete.
    std::map<LayerRegion *, double> required;
    for (const auto &[key, volumes] : transitions) {
        const auto capacities = capacities_for(key);
        size_t next = 0;
        for (double volume : volumes)
            while (volume > 0. && next < capacities.size()) {
                const auto &[region, capacity] = capacities[next++];
                if (capacity > 0.) required[region] += std::min(volume, capacity);
                volume -= capacity;
            }
    }
    for (PrintObject *object : objects) {
        for (Layer *layer : object->layers()) {
            bool rebuild = false;
            for (LayerRegion *region : layer->regions()) {
                auto &plan = region->purge_inner_wall_plan;
                plan.target_volume_mm3 = required[region] / std::max(size_t(1), object->instances().size());
                if (plan.target_volume_mm3 <= EPSILON && plan.extra_loops > 0) {
                    plan.extra_loops = 0;
                    rebuild = true;
                }
            }
            if (rebuild) layer->make_perimeters();
        }
        support_stack(object);
    }

    for (PrintObject *object : objects) {
        for (Layer *layer : object->layers()) {
            if (layer->lower_layer) {
                Polylines added;
                for (LayerRegion *region : layer->regions()) append(added, added_wall_centerlines(*region));
                if (!added.empty())
                    for (LayerRegion *below : layer->lower_layer->regions())
                        visit_perimeter_loops(below->perimeters, [&](ExtrusionLoop &loop) {
                            if (usable_inner_foundation(loop) &&
                                !intersection_pl(added, loop.polygons_covered_by_width(float(SCALED_EPSILON))).empty())
                                loop.purge_support = true;
                        });
            }
        }
        for (Layer *layer : object->layers())
            for (LayerRegion *region : layer->regions()) isolate_purge_loops(*region, coverage.at(region));
        restart_dependents(*object);
        object->infill();
        object->ironing();
        if (object->need_z_contouring()) object->contour_z();
        object->generate_support_material();
        object->detect_overhangs_for_lift();
        object->estimate_curled_extrusions();
    }
    // First-layer support propagation can change skirt/brim coverage too.
    {
        std::scoped_lock lock(state_mutex());
        invalidate_steps_for_recompute({psSkirtBrim, psGCodeExport});
    }
    {
        std::scoped_lock lock(m_inner_wall_purge_mutex);
        m_inner_wall_purge_transitions.clear();
    }
}

void Print::record_inner_wall_purge(double z, unsigned int old_filament, unsigned int new_filament,
                                    const PurgeVolumeAllocation &allocation, const PrintObject *object, int copy)
{
    auto stored = allocation;
    if (has_wipe_tower() && wipe_tower_type() == WipeTowerType::Type2) {
        const double minimum = m_config.filament_minimal_purge_on_wipe_tower.get_at(new_filament);
        stored.requested += minimum;
        stored.remaining += minimum;
    }
    std::scoped_lock lock(m_inner_wall_purge_mutex);
    m_inner_wall_purge_transitions[{object, copy, z, old_filament, new_filament}] = stored;
}

void Print::plan_towerless_inner_wall_purge(LayerTools &layer, unsigned int &current, const PrintObject *object, int copy)
{
    {
        std::scoped_lock lock(m_inner_wall_purge_mutex);
        for (auto it = m_inner_wall_purge_transitions.begin(); it != m_inner_wall_purge_transitions.end();) {
            const auto &[o, c, z, old_id, new_id] = it->first;
            if (o == object && c == copy && std::abs(z - layer.print_z) < EPSILON) it = m_inner_wall_purge_transitions.erase(it);
            else ++it;
        }
    }
    auto &wiping = layer.wiping_extrusions();
    wiping.reset_overrides(&layer);
    for (unsigned int next : layer.extruders) {
        if (next != current) wiping.mark_wiping_extrusions(*this, current, next, purge_volume_for_transition(current, next), object, copy);
        current = next;
    }
    wiping.ensure_perimeters_infills_order(*this, object, copy);
}

PurgeInnerWallStatistics Print::inner_wall_purge_statistics() const
{
    PurgeInnerWallStatistics stats;
    std::scoped_lock lock(m_inner_wall_purge_mutex);
    for (const auto &[key, allocation] : m_inner_wall_purge_transitions) {
        stats.requested += allocation.requested;
        stats.inner_walls += allocation.inner_walls;
        stats.other += allocation.other;
        if (has_wipe_tower()) stats.tower += allocation.remaining;
        else {
            stats.reduced += allocation.remaining;
            if (allocation.remaining > EPSILON) {
                stats.max_shortfall = std::max(stats.max_shortfall, allocation.remaining);
                stats.affected_layers.emplace(std::get<0>(key), std::get<1>(key), std::get<2>(key));
                stats.affected_pairs.emplace(std::get<3>(key), std::get<4>(key));
            }
        }
    }
    return stats;
}

void Print::warn_about_reduced_inner_wall_purge()
{
    const auto stats = inner_wall_purge_statistics();
    if (stats.reduced <= EPSILON) return;
    std::ostringstream message;
    message << I18N::translate(L("Prime tower is disabled, and the selected objects do not have enough safe inner-wall volume to contain all purge material. OrcaSlicer reduced purge volume for some filament transitions. This may cause color/material contamination. A prime tower is strongly recommended; enable one or add a larger purge object."));
    message << std::fixed << std::setprecision(2) << "\n" << stats.affected_layers.size() << " layers; "
            << stats.requested << " mm3 requested, " << stats.inner_walls + stats.other << " mm3 absorbed, "
            << stats.reduced << " mm3 reduced; maximum shortfall " << stats.max_shortfall << " mm3.\n";
    for (const auto &[old_id, new_id] : stats.affected_pairs) message << old_id + 1 << " -> " << new_id + 1 << "; ";
    active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, message.str(), PrintStateBase::SlicingReducedInnerWallPurge);
}

} // namespace Slic3r
