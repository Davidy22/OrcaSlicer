#include "ExtrusionEntityCollection.hpp"
#include "ExtrusionEntity.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "ShortestPath.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

void filter_by_extrusion_role_in_place(ExtrusionEntitiesPtr &extrusion_entities, ExtrusionRole role)
{
	if (role != erMixed) {
		auto first  = extrusion_entities.begin();
		auto last   = extrusion_entities.end();
        extrusion_entities.erase(
            std::remove_if(first, last, [&role](const ExtrusionEntity* ee) {
                return ee->role() != role; }),
            last);
	}
}

ExtrusionEntityCollection::ExtrusionEntityCollection(const ExtrusionPaths &paths)
    : no_sort(false)
{
    this->append(paths);
}

ExtrusionEntityCollection& ExtrusionEntityCollection::operator=(const ExtrusionEntityCollection &other)
{
    clear();
    this->entities      = other.entities;
    for (size_t i = 0; i < this->entities.size(); ++i)
        this->entities[i] = this->entities[i]->clone();
    this->no_sort       = other.no_sort;
    return *this;
}

void ExtrusionEntityCollection::swap(ExtrusionEntityCollection &c)
{
    std::swap(this->entities, c.entities);
    std::swap(this->no_sort, c.no_sort);
}

void ExtrusionEntityCollection::clear()
{
	for (size_t i = 0; i < this->entities.size(); ++i)
		delete this->entities[i];
    this->entities.clear();
}

ExtrusionEntityCollection::operator ExtrusionPaths() const
{
    ExtrusionPaths paths;
    for (const ExtrusionEntity *ptr : this->entities) {
        if (const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(ptr))
            paths.push_back(*path);
    }
    return paths;
}

ExtrusionEntity *ExtrusionEntityCollection::clone() const
{
    return new ExtrusionEntityCollection(*this);
}

void ExtrusionEntityCollection::reverse()
{
    for (ExtrusionEntity *ptr : this->entities)
        // Don't reverse it if it's a loop, as it doesn't change anything in terms of elements ordering
        // and caller might rely on winding order
        if (! ptr->is_loop())
        	ptr->reverse();
    std::reverse(this->entities.begin(), this->entities.end());
}

void ExtrusionEntityCollection::replace(size_t i, const ExtrusionEntity &entity)
{
    delete this->entities[i];
    this->entities[i] = entity.clone();
}

void ExtrusionEntityCollection::remove(size_t i)
{
    delete this->entities[i];
    this->entities.erase(this->entities.begin() + i);
}

ExtrusionEntityCollection ExtrusionEntityCollection::chained_path_from(const ExtrusionEntitiesPtr& extrusion_entities, const Point &start_near, ExtrusionRole role)
{
	// Return a filtered copy of the collection.
    ExtrusionEntityCollection out;
    out.entities = filter_by_extrusion_role(extrusion_entities, role);
	// Clone the extrusion entities.
	for (auto &ptr : out.entities)
		ptr = ptr->clone();
	chain_and_reorder_extrusion_entities(out.entities, &start_near);
    return out;
}

void ExtrusionEntityCollection::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    for (const ExtrusionEntity *entity : this->entities)
        entity->polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionEntityCollection::polygons_covered_by_spacing(Polygons &out, const float scaled_epsilon) const
{
    for (const ExtrusionEntity *entity : this->entities)
        entity->polygons_covered_by_spacing(out, scaled_epsilon);
}

// Recursively count paths and loops contained in this collection.
size_t ExtrusionEntityCollection::items_count() const
{
    size_t count = 0;
    for (const ExtrusionEntity *entity : this->entities)
        if (entity->is_collection())
            count += static_cast<const ExtrusionEntityCollection*>(entity)->items_count();
        else
            ++ count;
    return count;
}

// Returns a single vector of pointers to all non-collection items contained in this one.
ExtrusionEntityCollection ExtrusionEntityCollection::flatten(bool preserve_ordering) const
{
	struct Flatten {
		Flatten(bool preserve_ordering) : preserve_ordering(preserve_ordering) {}
		ExtrusionEntityCollection out;
		bool   					  preserve_ordering;
		void recursive_do(const ExtrusionEntityCollection &collection) {
		    if (collection.no_sort && preserve_ordering) {
		    	// Don't flatten whatever happens below this level.
		    	out.append(collection);
		    } else {
				for (const ExtrusionEntity *entity : collection.entities)
					if (entity->is_collection())
						this->recursive_do(*static_cast<const ExtrusionEntityCollection*>(entity));
					else
						out.append(*entity);
			}
		}
	} flatten(preserve_ordering);

	flatten.recursive_do(*this);
    return flatten.out;
}

double ExtrusionEntityCollection::min_mm3_per_mm() const
{
    double min_mm3_per_mm = std::numeric_limits<double>::max();
    for (const ExtrusionEntity *entity : this->entities)
        min_mm3_per_mm = std::min(min_mm3_per_mm, entity->min_mm3_per_mm());
    return min_mm3_per_mm;
}

// Fail closed: an override must never recolor an external wall, a thin wall,
// gap fill or an untagged path sharing a collection with a purge candidate.
bool ExtrusionEntityCollection::is_purge_inner_wall_candidate() const
{
    return !entities.empty() && std::all_of(entities.begin(), entities.end(), [](const ExtrusionEntity *entity) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            return collection->is_purge_inner_wall_candidate();
        const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity);
        return loop != nullptr && loop->is_purge_inner_wall_candidate && loop->inset_idx > 0 && !loop->paths.empty() &&
               std::all_of(loop->paths.begin(), loop->paths.end(), [](const ExtrusionPath &path) {
                   return path.role() == erPerimeter || path.role() == erOverhangPerimeter;
               });
    });
}

void ExtrusionEntityCollection::split_purge_inner_walls()
{
    ExtrusionEntityCollection separated;
    // Move, rather than clone, the leaves so the allocator, seam placer and
    // G-code writer all see the same wall entities. Depth-first traversal
    // preserves the generators' ordering, including deferred overhang loops.
    const auto separate = [&separated](auto &&self, std::unique_ptr<ExtrusionEntity> entity) -> void {
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity.get())) {
            for (ExtrusionEntity *&child : collection->entities)
                self(self, std::unique_ptr<ExtrusionEntity>(std::exchange(child, nullptr)));
        } else {
            auto wall = std::make_unique<ExtrusionEntityCollection>();
            wall->entities.emplace_back(entity.get());
            entity.release();
            separated.entities.emplace_back(wall.get());
            wall.release();
        }
    };
    for (ExtrusionEntity *&entity : entities)
        separate(separate, std::unique_ptr<ExtrusionEntity>(std::exchange(entity, nullptr)));
    entities.swap(separated.entities);
}

std::vector<ExtrusionEntityCollection::PurgeInnerWall> ExtrusionEntityCollection::purge_inner_walls() const
{
    std::vector<const ExtrusionEntityCollection *> collections;
    for (const ExtrusionEntity *entity : entities)
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            collections.push_back(collection);
    return purge_inner_walls(collections);
}

std::vector<ExtrusionEntityCollection::PurgeInnerWall> ExtrusionEntityCollection::purge_inner_walls(
    const std::vector<const ExtrusionEntityCollection *> &collections)
{
    struct Wall {
        const ExtrusionEntityCollection *collection;
        Polygon polygon;
        int inset;
        bool hole;
    };
    std::vector<Wall> walls;
    for (const auto *collection : collections) {
        // split_purge_inner_walls() guarantees one loop per collection. Fail
        // closed if a pipeline plugin replaces it with an unsplit group.
        if (collection == nullptr || collection->entities.size() != 1 || !collection->is_purge_inner_wall_candidate())
            continue;
        const auto *loop = dynamic_cast<const ExtrusionLoop *>(collection->entities.front());
        if (loop == nullptr || std::any_of(loop->paths.begin(), loop->paths.end(), [](const ExtrusionPath &path) {
                return path.polyline.points.size() < 2;
            }))
            continue;
        bool closed = loop->paths.front().first_point() == loop->paths.back().last_point();
        for (size_t i = 1; i < loop->paths.size() && closed; ++i)
            closed = loop->paths[i - 1].last_point() == loop->paths[i].first_point();
        if (!closed)
            continue;
        Polygon polygon = loop->polygon();
        if (polygon.points.size() >= 3)
            walls.push_back({collection, std::move(polygon), loop->inset_idx, (loop->loop_role() & elrHole) != 0});
    }
    std::stable_sort(walls.begin(), walls.end(), [](const Wall &a, const Wall &b) { return a.inset < b.inset; });

    std::vector<PurgeInnerWall> result;
    std::vector<size_t> shells;
    for (size_t i = 0; i < walls.size(); ++i) {
        const Wall &wall = walls[i];
        PurgeInnerWall candidate{wall.collection, {}};
        for (size_t shell_idx : shells) {
            const Wall &shell = walls[shell_idx];
            if (shell.inset >= wall.inset || shell.hole != wall.hole)
                continue;
            // Contours shrink inward; hole walls expand into the material.
            // A deeper hole loop may surround multiple holes after they merge:
            // keep every corresponding native shell, without double-counting it.
            if (wall.hole ? wall.polygon.contains(shell.polygon.first_point()) :
                            shell.polygon.contains(wall.polygon.first_point()))
                candidate.shells.push_back(shell.collection);
        }
        if (candidate.shells.empty()) {
            shells.push_back(i);
            candidate.shells.push_back(wall.collection);
        }
        result.push_back(std::move(candidate));
    }
    for (size_t i = 0; i < walls.size(); ++i) {
        if (result[i].is_shell())
            continue;
        for (size_t shell_idx : shells) {
            const Wall &shell = walls[shell_idx];
            if (shell.inset >= walls[i].inset || shell.hole == walls[i].hole)
                continue;
            // Deeper contours/holes can merge. Preserve both sides of the
            // material, not only the boundary matching this loop's role.
            if (walls[i].hole ? shell.polygon.contains(walls[i].polygon.first_point()) :
                                walls[i].polygon.contains(shell.polygon.first_point()))
                result[i].shells.push_back(shell.collection);
        }
    }
    return result;
}

// Check if this collection contains existing (non-generated) purge walls.
bool ExtrusionEntityCollection::is_existing_purge_wall() const
{
    for (const ExtrusionEntity *entity : entities) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            if (collection->is_existing_purge_wall())
                return true;
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
            if (loop->is_purge_inner_wall_candidate && !loop->generated_for_purge)
                return true;
        }
    }
    return false;
}

int ExtrusionEntityCollection::get_max_inset_idx() const
{
    int max_idx = -1;
    for (const ExtrusionEntity *entity : entities) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            max_idx = std::max(max_idx, collection->get_max_inset_idx());
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
            if (loop->is_purge_inner_wall_candidate)
                max_idx = std::max(max_idx, loop->inset_idx);
    }
    return max_idx;
}

}
