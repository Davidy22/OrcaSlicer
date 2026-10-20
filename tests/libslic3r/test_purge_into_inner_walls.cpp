#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/PurgeIntoInnerWalls.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"

#include <map>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <random>
#include <utility>

using namespace Slic3r;

namespace {
ExtrusionLoop purge_test_loop(ExtrusionRole role, int inset, bool generated = false)
{
    ExtrusionPath path(role);
    path.mm3_per_mm = 0.1;
    path.width = 0.4f;
    path.height = 0.2f;
    path.inset_idx = inset;
    path.polyline = Polyline3(Polyline(Points{
        Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 10.),
        Point::new_scale(0., 10.), Point::new_scale(0., 0.)}));
    ExtrusionLoop loop(std::move(path));
    loop.inset_idx = inset;
    loop.is_purge_inner_wall_candidate = inset > 0;
    loop.generated_for_purge = generated;
    return loop;
}

ExtrusionLoop native_shell_test_wall(int inset, double min_x, double min_y, double max_x, double max_y, bool hole = false)
{
    ExtrusionLoop loop = purge_test_loop(inset == 0 ? erExternalPerimeter : erPerimeter, inset);
    loop.paths.front().polyline = Polyline3(Polyline(Points{
        Point::new_scale(min_x, min_y), Point::new_scale(max_x, min_y), Point::new_scale(max_x, max_y),
        Point::new_scale(min_x, max_y), Point::new_scale(min_x, min_y)}));
    loop.set_loop_role(hole ? elrHole : elrDefault);
    return loop;
}
std::vector<ExtrusionEntityCollection::PurgeInnerWall> purge_test_walls(const ExtrusionEntityCollection &perimeters)
{
    std::vector<const ExtrusionEntityCollection *> collections;
    for (const ExtrusionEntity *entity : perimeters.entities)
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            collections.push_back(collection);
    return ExtrusionEntityCollection::purge_inner_walls(collections);
}
} // namespace

TEST_CASE("Wall collections distinguish existing and generated purge loops", "[PurgeIntoInnerWalls]")
{
    ExtrusionEntityCollection collection;
    REQUIRE_FALSE(collection.is_purge_inner_wall_candidate());
    REQUIRE_FALSE(collection.is_existing_purge_wall());
    REQUIRE(collection.get_max_inset_idx() == -1);

    collection.append(purge_test_loop(erPerimeter, 2));
    REQUIRE(collection.is_purge_inner_wall_candidate());
    REQUIRE(collection.is_existing_purge_wall());
    REQUIRE(collection.get_max_inset_idx() == 2);

    collection.append(purge_test_loop(erPerimeter, 3, true));
    REQUIRE(collection.is_purge_inner_wall_candidate());
    REQUIRE(collection.is_existing_purge_wall());
    REQUIRE(collection.get_max_inset_idx() == 3);
}

TEST_CASE("A mixed outer and inner wall collection is not a purge target", "[PurgeIntoInnerWalls][Regression]")
{
    ExtrusionEntityCollection island;
    island.append(purge_test_loop(erExternalPerimeter, 0));
    island.append(purge_test_loop(erPerimeter, 1));
    REQUIRE_FALSE(island.is_purge_inner_wall_candidate());

    ExtrusionEntityCollection nested;
    nested.append(std::move(island));
    REQUIRE_FALSE(nested.is_purge_inner_wall_candidate());
}

TEST_CASE("Purge target validation rejects visible walls even with an incorrect tag", "[PurgeIntoInnerWalls][Regression]")
{
    const ExtrusionRole role = GENERATE(erExternalPerimeter, erOverhangPerimeter, erGapFill, erTopSolidInfill);
    const int inset = GENERATE(0, 1);
    CAPTURE(role, inset);
    ExtrusionLoop loop = purge_test_loop(role, inset);
    loop.is_purge_inner_wall_candidate = true;
    ExtrusionEntityCollection collection;
    collection.append(std::move(loop));
    // Only an overhang belonging to an inner wall is eligible; external
    // overhangs share its role but have inset 0 and must stay protected.
    const bool eligible = role == erOverhangPerimeter && inset > 0;
    CHECK(collection.is_purge_inner_wall_candidate() == eligible);
}

TEST_CASE("Splitting purge walls preserves geometry and isolates outer walls", "[PurgeIntoInnerWalls][Regression]")
{
    ExtrusionEntityCollection island;
    island.append(purge_test_loop(erPerimeter, 2, true));
    island.append(purge_test_loop(erExternalPerimeter, 0));
    ExtrusionEntityCollection nested;
    nested.append(purge_test_loop(erPerimeter, 1));
    nested.append(purge_test_loop(erOverhangPerimeter, 0)); // outer overhang
    island.append(std::move(nested));
    // Arachne can also emit open paths: these are not tagged purge loops.
    island.append(purge_test_loop(erGapFill, 0).paths.front());

    ExtrusionEntityCollection perimeters;
    perimeters.append(std::move(island));
    const auto &original_island = static_cast<const ExtrusionEntityCollection &>(*perimeters.entities.front());
    const auto &original_nested = static_cast<const ExtrusionEntityCollection &>(*original_island.entities[2]);
    const std::vector<const ExtrusionEntity *> original_walls{
        original_island.entities[0], original_island.entities[1], original_nested.entities[0],
        original_nested.entities[1], original_island.entities[3]};
    const double original_volume = perimeters.total_volume();
    const double inner_volume = original_walls[0]->total_volume() + original_walls[2]->total_volume();

    // A second split is harmless and keeps the same entity identities.
    for (int pass = 0; pass < 2; ++pass) {
        CAPTURE(pass);
        perimeters.split_purge_inner_walls();
        REQUIRE(perimeters.size() == original_walls.size());
        double capacity = 0.;
        for (size_t i = 0; i < original_walls.size(); ++i) {
            const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(perimeters.entities[i]);
            REQUIRE(wall != nullptr);
            REQUIRE(wall->size() == 1);
            CHECK(wall->entities.front() == original_walls[i]);
            const bool eligible = i == 0 || i == 2;
            CHECK(wall->is_purge_inner_wall_candidate() == eligible);
            if (wall->is_purge_inner_wall_candidate())
                capacity += wall->total_volume();
        }
        CHECK_THAT(capacity, Catch::Matchers::WithinAbs(inner_volume, 1e-6));
        CHECK_THAT(perimeters.total_volume(), Catch::Matchers::WithinAbs(original_volume, 1e-6));
    }
    const auto &extra_wall = static_cast<const ExtrusionEntityCollection &>(*perimeters.entities[0]);
    CHECK_FALSE(extra_wall.is_existing_purge_wall());
    CHECK(extra_wall.get_max_inset_idx() == 2);
    const auto &existing_wall = static_cast<const ExtrusionEntityCollection &>(*perimeters.entities[2]);
    CHECK(existing_wall.is_existing_purge_wall());
    CHECK(existing_wall.get_max_inset_idx() == 1);
}

TEST_CASE("Purge settings register once in their global and object scopes", "[PurgeIntoInnerWalls]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    // Check that the option exists
    const ConfigOption* purge_opt = config.option("purge_into_inner_walls");
    REQUIRE(purge_opt != nullptr);
    REQUIRE_FALSE(purge_opt->getBool());

    // Demand, not a preset loop-count cap, bounds the extra walls.
    REQUIRE(config.option("purge_inner_walls_max_extra_loops") == nullptr);

    // Duplicating a key in both scopes crashes FullPrintConfig registration.
    REQUIRE(PrintConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE_FALSE(PrintConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE_FALSE(PrintConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE(PrintObjectConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().purge_into_inner_walls_preferred.getBool());

    const auto full_keys = FullPrintConfig::defaults().keys();
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_into_inner_walls") == 1);
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_inner_walls_max_extra_loops") == 0);
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_into_inner_walls_preferred") == 1);
}

// Test the flush volume estimate shared by the wall-growth planning
// (PrintObject::make_perimeters), the per-layer proportional reduction
// (WipingExtrusions::mark_wiping_extrusions) and the allocation planner
// (Print::_plan_purge_into_inner_walls).
TEST_CASE("Purge flush volume estimate", "[PurgeIntoInnerWalls]")
{
    const size_t nozzles = GENERATE(size_t{0}, size_t{1}, size_t{2}, size_t{3});
    CAPTURE(nozzles);
    PrintConfig config = PrintConfig::defaults();
    config.nozzle_diameter.values.assign(nozzles, 0.4);
    config.flush_volumes_matrix.values = { 0., 100., 200., 0. }; // first nozzle: 2x2, row-major
    config.flush_volumes_matrix.values.resize(4 * std::max(nozzles, size_t{1}), 999.);
    config.flush_multiplier.values     = { 1.0 };
    config.prime_volume.value          = 42.;
    config.purge_in_prime_tower.value        = true;
    config.single_extruder_multi_material.value = true;

    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(100., 1e-4));
    REQUIRE_THAT(estimate_flush_volume(config, 1, 0), Catch::Matchers::WithinAbs(200., 1e-4));
    REQUIRE_THAT(estimate_flush_volume(config, 1, 1), Catch::Matchers::WithinAbs(0., 1e-4));

    config.flush_multiplier.values = { 2.0 };
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(200., 1e-4));

    // Out-of-range filament falls back to the prime volume.
    REQUIRE_THAT(estimate_flush_volume(config, 0, 5), Catch::Matchers::WithinAbs(42., 1e-4));
    if (nozzles > 1) {
        // Integer partitioning ignores a remainder after the complete nozzle slices.
        config.flush_volumes_matrix.values.push_back(1234.);
        REQUIRE_THAT(estimate_flush_volume(config, 1, 0), Catch::Matchers::WithinAbs(400., 1e-4));
    }
    // Without purging volumes on a single-extruder multi-material printer,
    // every toolchange purges the plain prime volume (belt semantics).
    config.purge_in_prime_tower.value = false;
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(42., 1e-4));
    REQUIRE_THAT(estimate_flush_volume(config, 1, 1), Catch::Matchers::WithinAbs(0., 1e-4));

    // Empty or non-square first-nozzle matrices also retain the prime fallback.
    config.purge_in_prime_tower.value = true;
    config.flush_volumes_matrix.values.resize(3 * std::max(nozzles, size_t{1}));
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(42., 1e-4));
    config.flush_volumes_matrix.values.clear();
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(42., 1e-4));
}

// The estimate decides how much a purge target is asked to absorb, so it has to
// read the same multiplier the prime tower and ToolOrdering's flush matrices do.
// Fast purge mode carries its own flush_multiplier_fast; every other mode keeps
// the default one, and an unset fast vector must not be read out of bounds.
TEST_CASE("Purge flush volume estimate follows the prime volume mode", "[PurgeIntoInnerWalls]")
{
    PrintConfig config = PrintConfig::defaults();
    config.nozzle_diameter.values.assign(1, 0.4);
    config.flush_volumes_matrix.values            = { 0., 100., 200., 0. };
    config.flush_multiplier.values                = { 2.0 };
    config.flush_multiplier_fast.values           = { 0.5 };
    config.prime_volume.value                     = 42.;
    config.purge_in_prime_tower.value             = true;
    config.single_extruder_multi_material.value   = true;

    // Default mode is inert: the fast vector is ignored.
    config.prime_volume_mode.value = PrimeVolumeMode::pvmDefault;
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(200., 1e-4));

    // Saving mode reduces the prime volume, but this estimate only feeds the
    // flush matrix, which keeps the default multiplier.
    config.prime_volume_mode.value = PrimeVolumeMode::pvmSaving;
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(200., 1e-4));

    // Fast mode swaps in flush_multiplier_fast instead of the default.
    config.prime_volume_mode.value = PrimeVolumeMode::pvmFast;
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(50., 1e-4));
    REQUIRE_THAT(estimate_flush_volume(config, 1, 0), Catch::Matchers::WithinAbs(100., 1e-4));

    // A half-migrated profile can leave the fast vector empty; fall back to the
    // default multiplier instead of reading index 0 out of bounds.
    config.flush_multiplier_fast.values.clear();
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(200., 1e-4));
}

TEST_CASE("Purge seam samples follow loop arc length rather than vertex indices", "[PurgeIntoInnerWalls][Regression]")
{
    const Points rectangle{Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                           Point::new_scale(20., 10.), Point::new_scale(0., 10.)};
    const std::vector<std::pair<double, Point>> samples{
        {0., Point::new_scale(0., 0.)}, {0.25, Point::new_scale(15., 0.)},
        {0.5, Point::new_scale(20., 10.)}, {0.75, Point::new_scale(5., 10.)},
        {55. / 60., Point::new_scale(0., 5.)}, {1., Point::new_scale(0., 0.)},
    };
    for (const auto &[fraction, expected] : samples) {
        CAPTURE(fraction);
        const auto point = PurgeIntoInnerWalls::seam_point(rectangle, fraction);
        REQUIRE(point.has_value());
        CHECK(point->distance_to(expected) <= scaled<double>(0.000002));
    }
}

TEST_CASE("Purge seam sampling tolerates duplicate vertices and rejects empty loops", "[PurgeIntoInnerWalls][Regression]")
{
    const Points rectangle{Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                           Point::new_scale(20., 10.), Point::new_scale(0., 10.)};
    Points subdivided;
    for (int x = 0; x <= 20; ++x)
        subdivided.push_back(Point::new_scale(double(x), 0.));
    subdivided.insert(subdivided.end(), {rectangle[2], rectangle[3], rectangle[0], rectangle[0]});
    for (double fraction : {0., 0.01, 0.25, 0.5, 0.75, 0.99, 1.}) {
        CAPTURE(fraction);
        const auto sparse = PurgeIntoInnerWalls::seam_point(rectangle, fraction);
        const auto dense = PurgeIntoInnerWalls::seam_point(subdivided, fraction);
        REQUIRE(sparse.has_value());
        REQUIRE(dense.has_value());
        CHECK(sparse->distance_to(*dense) <= scaled<double>(0.000002));
    }
    CHECK_FALSE(PurgeIntoInnerWalls::seam_point({}, 0.5).has_value());
    CHECK_FALSE(PurgeIntoInnerWalls::seam_point(Points(4, rectangle.front()), 0.5).has_value());
    CHECK_FALSE(PurgeIntoInnerWalls::seam_point(rectangle, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(PurgeIntoInnerWalls::seam_point(rectangle, std::numeric_limits<double>::infinity()).has_value());
}

TEST_CASE("Purge seam sampling prefers supported spans without piling up at endpoints", "[PurgeIntoInnerWalls][Regression]")
{
    const Points rectangle{Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                           Point::new_scale(20., 10.), Point::new_scale(0., 10.)};
    const std::vector<bool> bottom_only{true, false, false, false};
    for (double fraction : {0., 0.25, 0.5, 0.75, 1.}) {
        CAPTURE(fraction);
        const auto point = PurgeIntoInnerWalls::seam_point(rectangle, fraction, bottom_only);
        REQUIRE(point.has_value());
        CHECK(point->distance_to(Point::new_scale(20. * fraction, 0.)) <= scaled<double>(0.000002));
    }
    // Two separated supported spans must not acquire a fictitious join or
    // concentrate starts at the nearest supported endpoint.
    const auto opposite = PurgeIntoInnerWalls::seam_point(rectangle, 0.75, {true, false, true, false});
    REQUIRE(opposite.has_value());
    CHECK(opposite->distance_to(Point::new_scale(10., 10.)) <= scaled<double>(0.000002));
    const auto all_overhang = PurgeIntoInnerWalls::seam_point(rectangle, 0.25, std::vector<bool>(4, false));
    REQUIRE(all_overhang.has_value());
    CHECK(all_overhang->distance_to(Point::new_scale(15., 0.)) <= scaled<double>(0.000002));
    Points duplicate = rectangle;
    duplicate.insert(duplicate.begin(), rectangle.front());
    const auto zero_preferred = PurgeIntoInnerWalls::seam_point(duplicate, 0.25, {true, false, false, false, false});
    REQUIRE(zero_preferred.has_value());
    CHECK(zero_preferred->distance_to(Point::new_scale(15., 0.)) <= scaled<double>(0.000002));
    CHECK_FALSE(PurgeIntoInnerWalls::seam_point(rectangle, 0.5, {true}).has_value());
    // Fully supported loops need no allocated mask. Omitting it must retain
    // exactly the same starts, including edge boundaries and loop closure.
    for (double fraction : {0., 0.01, 0.25, 0.5, 0.75, 0.99, 1.}) {
        CAPTURE(fraction);
        const auto unmasked = PurgeIntoInnerWalls::seam_point(rectangle, fraction);
        const auto supported = PurgeIntoInnerWalls::seam_point(rectangle, fraction, std::vector<bool>(4, true));
        REQUIRE(unmasked.has_value());
        REQUIRE(supported.has_value());
        CHECK(*unmasked == *supported);
    }
}

TEST_CASE("Purge seam draws scatter identical loops and replay after an export reset", "[PurgeIntoInnerWalls][Regression]")
{
    const Points rectangle{Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                           Point::new_scale(20., 10.), Point::new_scale(0., 10.)};
    std::minstd_rand rng;
    Points seams;
    for (size_t loop = 0; loop < 32; ++loop) {
        const auto point = PurgeIntoInnerWalls::seam_point(rectangle,
            std::generate_canonical<double, std::numeric_limits<double>::digits>(rng));
        REQUIRE(point.has_value());
        CHECK(point->x() >= scale_(0.));
        CHECK(point->x() <= scale_(20.));
        CHECK(point->y() >= scale_(0.));
        CHECK(point->y() <= scale_(10.));
        const coord_t boundary_distance = std::min({point->x(), scaled<coord_t>(20.) - point->x(),
                                                     point->y(), scaled<coord_t>(10.) - point->y()});
        CHECK(boundary_distance == 0);
        seams.push_back(*point);
    }
    CHECK(std::any_of(seams.begin(), seams.end(), [](const Point &p) { return p.x() < scale_(5.); }));
    CHECK(std::any_of(seams.begin(), seams.end(), [](const Point &p) { return p.x() > scale_(15.); }));
    CHECK(std::any_of(seams.begin(), seams.end(), [](const Point &p) { return p.y() < scale_(2.5); }));
    CHECK(std::any_of(seams.begin(), seams.end(), [](const Point &p) { return p.y() > scale_(7.5); }));

    rng.seed();
    for (const Point &expected : seams) {
        const auto replay = PurgeIntoInnerWalls::seam_point(rectangle,
            std::generate_canonical<double, std::numeric_limits<double>::digits>(rng));
        REQUIRE(replay.has_value());
        CHECK(*replay == expected);
    }
    std::sort(seams.begin(), seams.end());
    CHECK(std::unique(seams.begin(), seams.end()) == seams.end());
}

TEST_CASE("Purge wall tiers put preferred objects then matching exterior colors first", "[PurgeIntoInnerWalls][Regression]")
{
    using PurgeIntoInnerWalls::wall_tier;
    using PurgeIntoInnerWalls::wall_depth_priority;
    // The per-object checkbox wins even over a matching exterior color.
    CHECK(wall_tier(true, false) > wall_tier(false, true));
    // A matching exterior color outranks a foreign region: burning the purge
    // filament inside its own color cannot discolor the part, so that region is
    // exhausted before a foreign region gives up its native walls.
    CHECK(wall_tier(false, true) > wall_tier(false, false));
    CHECK(wall_tier(true, true) > wall_tier(true, false));

    // Inside a tier, equal volumes prefer the loop nearest a matching exterior
    // and the deepest loop for a foreign one, so the visible boundary keeps its
    // native color either way.
    CHECK(wall_depth_priority(true, 1, false) > wall_depth_priority(true, 3, true));
    CHECK(wall_depth_priority(false, 5, false) > wall_depth_priority(false, 2, true));
    // At equal depth an existing wall is reused before a purge-generated one.
    CHECK(wall_depth_priority(true, 2, true) > wall_depth_priority(true, 2, false));
    CHECK(wall_depth_priority(false, 2, true) > wall_depth_priority(false, 2, false));
}

TEST_CASE("Purge wall color matching outranks the volume fit inside a tier", "[PurgeIntoInnerWalls][Regression]")
{
    using PurgeIntoInnerWalls::wall_before;
    using PurgeIntoInnerWalls::pick_next_claim;
    const auto volumes = [](std::vector<float> values) {
        return [values = std::move(values)](size_t index) { return values.at(index); };
    };

    // A matching tier is claimed by distance to its own exterior, not by how
    // well a loop is sized: the wall the purge color continues through is worth
    // more than packing efficiency, so a huge nearest loop comes before a small
    // deep one.
    CHECK(wall_before(true, 1, false, 900.f, true, 4, false, 10.f));
    CHECK_FALSE(wall_before(true, 4, false, 10.f, true, 1, false, 900.f));
    // Equal depth falls back to volume, then existing-before-generated via the
    // depth key.
    CHECK(wall_before(true, 2, false, 10.f, true, 2, false, 20.f));
    CHECK(wall_before(true, 2, true, 20.f, true, 2, false, 10.f));

    // A foreign tier keeps best-fit volume ordering, deepest loop first on ties.
    CHECK(wall_before(false, 6, false, 10.f, false, 1, false, 20.f));
    CHECK(wall_before(false, 6, false, 10.f, false, 1, false, 10.f));
    CHECK_FALSE(wall_before(false, 1, false, 10.f, false, 6, false, 10.f));

    // The claim picker never reorders a matching tier...
    CHECK(pick_next_claim(4, true, volumes({2.f, 5.f, 40.f, 100.f}), 30.f) == 0);
    // ...but best-fits a foreign one.
    CHECK(pick_next_claim(4, false, volumes({2.f, 5.f, 40.f, 100.f}), 30.f) == 2);
    // An exhausted tier reports `count`, never a claimable slot.
    CHECK(pick_next_claim(0, true, volumes({}), 30.f) == 0);
    CHECK(pick_next_claim(0, false, volumes({}), 30.f) == 0);
}

TEST_CASE("Purge fills the smallest foreign wall that absorbs the remainder", "[PurgeIntoInnerWalls][Regression]")
{
    using PurgeIntoInnerWalls::pick_volume_fit;
    // Ascending volumes, as the allocator keeps a foreign tier. A matching tier
    // is claimed in color order instead, see pick_next_claim().
    const auto volumes = [](std::vector<float> values) {
        return [values = std::move(values)](size_t index) { return values.at(index); };
    };
    // The smallest loop that can absorb the whole remainder wins, so a small
    // flush never burns a large wall.
    CHECK(pick_volume_fit(4, volumes({2.f, 5.f, 40.f, 100.f}), 30.f) == 2);  // 40
    CHECK(pick_volume_fit(4, volumes({2.f, 5.f, 40.f, 100.f}), 5.f) == 1);   // exact fit
    CHECK(pick_volume_fit(4, volumes({2.f, 5.f, 40.f, 100.f}), 1.f) == 0);   // smallest available
    CHECK(pick_volume_fit(4, volumes({2.f, 5.f, 40.f, 100.f}), 0.f) == 0);
    // Nothing absorbs the remainder: fall back to the largest loop so packing
    // converges instead of stalling on the smallest.
    CHECK(pick_volume_fit(3, volumes({2.f, 5.f, 40.f}), 100.f) == 2);
    CHECK(pick_volume_fit(3, volumes({2.f, 5.f, 40.f}), 40.5f) == 2);
    // No candidates left.
    CHECK(pick_volume_fit(0, volumes({}), 10.f) == 0);
}

TEST_CASE("Purge wall speed ramps without exceeding existing speed or flow limits", "[PurgeIntoInnerWalls][Regression]")
{
    using PurgeIntoInnerWalls::limit_speed;
    using Catch::Matchers::WithinAbs;
    const double target = 60.;
    const std::vector<double> expected{19.8, 22.5, 27.48, 52.5, 52.5 + 50. / 60.};
    for (size_t wall = 0; wall < expected.size(); ++wall) {
        CAPTURE(wall);
        CHECK_THAT(limit_speed(180., target, wall, 0., 0.08), WithinAbs(expected[wall], 1e-8));
        CHECK_THAT(limit_speed(10., target, wall, 0., 0.08), WithinAbs(10., 1e-8));
        CHECK_THAT(limit_speed(180., target, wall, 1., 0.1), WithinAbs(10., 1e-8));
    }
    CHECK_THAT(limit_speed(180., target, 100, 0., 0.08), WithinAbs(target, 1e-8));
    CHECK_THAT(limit_speed(180., target, 100, 100., 0.08), WithinAbs(target, 1e-8));
    CHECK_THAT(limit_speed(180., target, 0, std::numeric_limits<double>::quiet_NaN(), 0.08),
               WithinAbs(expected.front(), 1e-8));
    // A zero-flow seam/travel fragment must not divide by zero.
    CHECK_THAT(limit_speed(180., target, 0, 1., 0.), WithinAbs(expected.front(), 1e-8));
}

TEST_CASE("Conflicting imported purge settings favor inner walls over the prime tower", "[PurgeIntoInnerWalls]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE_FALSE(config.option("purge_into_inner_walls")->getBool());
    REQUIRE_FALSE(config.option("enable_prime_tower")->getBool());

    config.set_key_value("purge_into_inner_walls", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.normalize_fdm_2(1, 2);
    REQUIRE(config.option("purge_into_inner_walls")->getBool());
    REQUIRE_FALSE(config.option("enable_prime_tower")->getBool());

    config.set_key_value("purge_into_inner_walls", new ConfigOptionBool(false));
    config.normalize_fdm_2(1, 2);
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.normalize_fdm_2(1, 2);
    REQUIRE_FALSE(config.option("purge_into_inner_walls")->getBool());
    REQUIRE(config.option("enable_prime_tower")->getBool());
}
