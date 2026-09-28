#include "ExtrusionEntityCollection.hpp"
#include "ShortestPath.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {

bool is_purge_inner_wall(const ExtrusionEntity &entity)
{
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        return loop->generated_for_purge && loop->purge_safe && loop->inset_idx > 0 && !loop->print_after_infill &&
               !loop->paths.empty() && std::all_of(loop->paths.begin(), loop->paths.end(), [](const auto &path) { return path.role() == erPerimeter; });
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
        return collection->entities.size() == 1 && is_purge_inner_wall(*collection->entities.front());
    return false;
}

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
    this->purge_geometry_locked = other.purge_geometry_locked;
    return *this;
}

void ExtrusionEntityCollection::swap(ExtrusionEntityCollection &c)
{
    std::swap(this->entities, c.entities);
    std::swap(this->no_sort, c.no_sort);
    std::swap(this->purge_geometry_locked, c.purge_geometry_locked);
}

void ExtrusionEntityCollection::clear()
{
	for (size_t i = 0; i < this->entities.size(); ++i)
		delete this->entities[i];
    this->entities.clear();
    this->purge_geometry_locked = false;
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

std::vector<const ExtrusionLoop *> ExtrusionEntityCollection::purge_inner_wall_candidates() const
{
    std::vector<const ExtrusionLoop *> candidates;
    const auto collect = [&candidates](const auto &self, const ExtrusionEntityCollection &collection) -> void {
        for (const ExtrusionEntity *entity : collection.entities) {
            if (const auto *nested = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
                self(self, *nested);
            } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                // role() on a loop reports only its FIRST path. Test every path
                // to avoid accepting loops that contain an overhang or bridge.
                if (loop->generated_for_purge && loop->inset_idx > 0 && !loop->print_after_infill && !loop->paths.empty() &&
                    std::all_of(loop->paths.begin(), loop->paths.end(), [](const ExtrusionPath &path) { return path.role() == erPerimeter; }))
                    candidates.push_back(loop);
            }
        }
    };
    collect(collect, *this);
    std::stable_sort(candidates.begin(), candidates.end(), [](const ExtrusionLoop *a, const ExtrusionLoop *b) {
        return a->inset_idx > b->inset_idx;
    });
    return candidates;
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

}
