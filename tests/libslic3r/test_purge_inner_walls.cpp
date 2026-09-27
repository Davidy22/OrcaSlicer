#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"

using namespace Slic3r;

namespace {
ExtrusionLoop purge_loop(int inset, ExtrusionRole role = erPerimeter)
{
    ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
    const coord_t side = scale_(10.);
    for (const Point &point : Points{Point(coord_t(0), coord_t(0)), Point(side, coord_t(0)), Point(side, side), Point(coord_t(0), coord_t(0))})
        path.polyline.append(Point3(point.x(), point.y(), coord_t(0)));
    ExtrusionLoop loop(path);
    loop.inset_idx = inset;
    loop.generated_for_purge = true;
    return loop;
}
}

TEST_CASE("Purge candidate discovery orders added walls innermost first", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    island.append(purge_loop(3));
    island.append(purge_loop(5));
    island.append(purge_loop(4));
    const auto candidates = island.purge_inner_wall_candidates();
    REQUIRE(candidates.size() == 3);
    REQUIRE(candidates[0]->inset_idx == 5);
    REQUIRE(candidates[1]->inset_idx == 4);
    REQUIRE(candidates[2]->inset_idx == 3);
    // Discovery does not reorder ordinary wall geometry or mutate ownership.
    REQUIRE(island.entities.front()->inset_idx == 3);
}

TEST_CASE("External and ordinary walls are never purge candidates", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    island.append(purge_loop(0));
    island.append(purge_loop(3, erExternalPerimeter));
    auto normal = purge_loop(4);
    normal.generated_for_purge = false;
    island.append(normal);
    REQUIRE(island.purge_inner_wall_candidates().empty());
}

TEST_CASE("A later overhang path excludes the whole purge loop", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    auto loop = purge_loop(3);
    loop.paths.push_back(purge_loop(3, erOverhangPerimeter).paths.front());
    REQUIRE(loop.role() == erPerimeter); // role() alone is not a sufficient filter.
    island.append(loop);
    REQUIRE(island.purge_inner_wall_candidates().empty());
}

TEST_CASE("Walls deferred until after infill cannot prime before normal geometry", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    auto loop = purge_loop(3);
    loop.print_after_infill = true;
    island.append(loop);
    REQUIRE(island.purge_inner_wall_candidates().empty());
}

TEST_CASE("Empty tagged loops and open paths are not purge wall candidates", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    auto loop = purge_loop(3);
    island.append(loop.paths.front());
    loop.paths.clear();
    island.append(loop);
    REQUIRE(island.purge_inner_wall_candidates().empty());
}

TEST_CASE("Purge provenance survives cloning flattening and moving nested collections", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection island;
    island.append(purge_loop(3));
    island.append(purge_loop(4));
    ExtrusionEntityCollection layer;
    layer.append(island);
    ExtrusionEntityCollection copy(layer);
    ExtrusionEntityCollection moved(std::move(copy));
    auto flattened = moved.flatten();
    const auto candidates = flattened.purge_inner_wall_candidates();
    REQUIRE(candidates.size() == 2);
    REQUIRE(candidates[0]->inset_idx == 4);
    REQUIRE(candidates[1]->inset_idx == 3);
    REQUIRE(candidates[0] != layer.purge_inner_wall_candidates()[0]);
    REQUIRE_THAT(candidates[0]->total_volume(), Catch::Matchers::WithinAbs(layer.purge_inner_wall_candidates()[0]->total_volume(), 1e-9));
}

TEST_CASE("A safe override requires an isolated verified ordinary perimeter", "[PurgeInnerWalls]")
{
    auto loop = purge_loop(3);
    REQUIRE_FALSE(is_purge_inner_wall(loop));
    loop.purge_safe = true;
    ExtrusionEntityCollection single;
    single.append(loop);
    REQUIRE(is_purge_inner_wall(single));
    single.append(purge_loop(0, erExternalPerimeter));
    REQUIRE_FALSE(is_purge_inner_wall(single));
    loop.paths.push_back(purge_loop(3, erOverhangPerimeter).paths.front());
    REQUIRE_FALSE(is_purge_inner_wall(loop));
}

TEST_CASE("Verified geometry locks survive collection copying and clear on regeneration", "[PurgeInnerWalls]")
{
    ExtrusionEntityCollection original;
    original.append(purge_loop(3));
    original.purge_geometry_locked = true;
    ExtrusionEntityCollection copied(original);
    REQUIRE(copied.purge_geometry_locked);
    ExtrusionEntityCollection assigned;
    assigned = original;
    REQUIRE(assigned.purge_geometry_locked);
    ExtrusionEntityCollection moved(std::move(copied));
    REQUIRE(moved.purge_geometry_locked);
    ExtrusionEntityCollection swapped;
    swapped.swap(moved);
    REQUIRE(swapped.purge_geometry_locked);
    REQUIRE_FALSE(moved.purge_geometry_locked);
    swapped.clear();
    REQUIRE_FALSE(swapped.purge_geometry_locked);
}

TEST_CASE("An ordinary inner foundation never becomes a purge override target", "[PurgeInnerWalls]")
{
    auto loop = purge_loop(2);
    loop.generated_for_purge = false;
    loop.purge_support = true;
    REQUIRE_FALSE(is_purge_inner_wall(loop));
    ExtrusionEntityCollection original;
    original.append(loop);
    ExtrusionEntityCollection copied(original);
    const auto *copy = dynamic_cast<const ExtrusionLoop *>(copied.entities.front());
    REQUIRE(copy != nullptr);
    REQUIRE(copy->purge_support);
    REQUIRE_FALSE(copy->generated_for_purge);
    REQUIRE_FALSE(is_purge_inner_wall(copied));
}
