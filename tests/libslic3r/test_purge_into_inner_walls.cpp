// Test cases for purge into inner walls feature
// Verifies that inner walls are properly tagged and extra walls are generated when needed

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

// Test ExtrusionEntityCollection purge candidate detection
TEST_CASE("ExtrusionEntityCollection purge candidate detection", "[PurgeIntoInnerWalls]")
{
    ExtrusionEntityCollection coll;
    
    // Initially no purge candidates
    REQUIRE(coll.is_purge_inner_wall_candidate() == false);
    REQUIRE(coll.is_existing_purge_wall() == false);
    REQUIRE(coll.get_max_inset_idx() == -1);
    
    // Add a purge candidate loop (existing wall).
    coll.append(purge_test_loop(erPerimeter, 2));

    // Verify detection
    REQUIRE(coll.is_purge_inner_wall_candidate() == true);
    REQUIRE(coll.is_existing_purge_wall() == true);
    REQUIRE(coll.get_max_inset_idx() == 2);
    
    // Add a generated purge wall (extra wall).
    coll.append(purge_test_loop(erPerimeter, 3, true));

    // Verify mixed detection
    REQUIRE(coll.is_purge_inner_wall_candidate() == true);
    REQUIRE(coll.is_existing_purge_wall() == true);  // still has existing walls
    REQUIRE(coll.get_max_inset_idx() == 3);  // max of both
    
    // Test volume calculation
    double volume = coll.total_volume();
    REQUIRE(volume > 0);
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

// Test that PrintConfig has purge_into_inner_walls option
TEST_CASE("PrintConfig purge_into_inner_walls option", "[PurgeIntoInnerWalls]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    
    // Check that the option exists
    const ConfigOption* purge_opt = config.option("purge_into_inner_walls");
    REQUIRE(purge_opt != nullptr);
    REQUIRE(purge_opt->getBool() == false);  // default is false

    // Extra wall loops are grown until the purge volume fits (or the layer
    // is solid), so the former loop-count cap option must not exist anymore.
    // (DynamicPrintConfig has no key whitelist, so absent def => nullptr.)
    REQUIRE(config.option("purge_inner_walls_max_extra_loops") == nullptr);

    // The feature toggle is global (PrintConfig) while the prioritization
    // toggle is per-object (PrintObjectConfig). Declaring the same key in both
    // classes would make FullPrintConfig register it twice (assert in
    // StaticCacheBase::opt_add) and direct member access ambiguous.
    REQUIRE(PrintConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE_FALSE(PrintConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE_FALSE(PrintConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE(PrintObjectConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE(PrintObjectConfig::defaults().purge_into_inner_walls_preferred.getBool() == false);

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
    PrintConfig config = PrintConfig::defaults();
    config.nozzle_diameter.values      = { 0.4 };
    config.flush_volumes_matrix.values = { 0., 100., 200., 0. }; // 2x2, row-major
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

    // Without purging volumes on a single-extruder multi-material printer,
    // every toolchange purges the plain prime volume (belt semantics).
    config.purge_in_prime_tower.value = false;
    REQUIRE_THAT(estimate_flush_volume(config, 0, 1), Catch::Matchers::WithinAbs(42., 1e-4));
    REQUIRE_THAT(estimate_flush_volume(config, 1, 1), Catch::Matchers::WithinAbs(0., 1e-4));
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

TEST_CASE("Purge wall priority favors preferred objects then the largest loop volumes", "[PurgeIntoInnerWalls][Regression]")
{
    using PurgeIntoInnerWalls::wall_priority;
    // The per-object checkbox wins even over a much larger native-color wall.
    CHECK(wall_priority(true, false, 5, false, 1.f) > wall_priority(false, true, 1, true, 100.f));
    // Within either preference tier, volume outranks color, depth and whether
    // the wall was generated for purge or already existed.
    CHECK(wall_priority(false, false, 3, false, 20.f) > wall_priority(false, true, 1, true, 1.f));
    CHECK(wall_priority(true, true, 4, false, 20.f) > wall_priority(true, true, 1, true, 1.f));
    CHECK(wall_priority(true, false, 2, false, 20.f) > wall_priority(true, false, 5, true, 1.f));
    CHECK(wall_priority(false, false, 2, false, 20.f) > wall_priority(false, false, 2, true, 1.f));
    // Equal volumes retain the existing color/depth/capacity tie-breakers.
    CHECK(wall_priority(false, true, 4, true, 20.f) > wall_priority(false, false, 8, true, 20.f));
    CHECK(wall_priority(false, true, 1, false, 20.f) > wall_priority(false, true, 3, true, 20.f));
    CHECK(wall_priority(false, false, 5, false, 20.f) > wall_priority(false, false, 2, true, 20.f));
    CHECK(wall_priority(false, false, 2, true, 20.f) > wall_priority(false, false, 2, false, 20.f));
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

// Test mutual exclusivity between purge_into_inner_walls and enable_prime_tower
TEST_CASE("Mutual exclusivity purge vs prime tower", "[PurgeIntoInnerWalls]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    
    // Initially both should be false
    REQUIRE(config.option("purge_into_inner_walls")->getBool() == false);
    REQUIRE(config.option("enable_prime_tower")->getBool() == false);
    
    // Enable purge mode
    config.set_key_value("purge_into_inner_walls", new ConfigOptionBool(true));
    REQUIRE(config.option("purge_into_inner_walls")->getBool() == true);
    
    // Try to enable prime tower - should be disabled by mutual exclusivity
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    
    // Run normalization which enforces mutual exclusivity
    config.normalize_fdm_2(1, 2);
    
    // Prime tower should be disabled when purge is enabled
    REQUIRE(config.option("purge_into_inner_walls")->getBool() == true);
    REQUIRE(config.option("enable_prime_tower")->getBool() == false);
    
    // Now disable purge mode
    config.set_key_value("purge_into_inner_walls", new ConfigOptionBool(false));
    config.normalize_fdm_2(1, 2);
    
    // Prime tower can now be enabled
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.normalize_fdm_2(1, 2);
    
    REQUIRE(config.option("purge_into_inner_walls")->getBool() == false);
    REQUIRE(config.option("enable_prime_tower")->getBool() == true);
}


TEST_CASE("Native shell reservations distinguish islands and hole boundaries", "[PurgeIntoInnerWalls][Regression]")
{
    const bool clockwise = GENERATE(false, true);
    ExtrusionEntityCollection perimeters;
    std::vector<ExtrusionLoop> loops{
        native_shell_test_wall(2, 1., 1., 19., 19.),
        native_shell_test_wall(1, 30., 0., 50., 20.),
        native_shell_test_wall(0, -1., -1., 21., 21.),
        native_shell_test_wall(2, 5., 5., 9., 9., true),
        native_shell_test_wall(1, 0., 0., 20., 20.),
        native_shell_test_wall(2, 31., 1., 49., 19.),
        native_shell_test_wall(1, 6., 6., 8., 8., true),
    };
    for (auto &loop : loops) {
        // Boundary identity comes from loop_role, not winding: Orca lets the
        // user reverse wall direction and overhangs may reverse it per layer.
        if (clockwise)
            loop.make_clockwise();
        else
            loop.make_counter_clockwise();
        perimeters.append(std::move(loop));
    }
    perimeters.split_purge_inner_walls();
    const auto walls = purge_test_walls(perimeters);
    REQUIRE(walls.size() == 6);
    size_t native_shells = 0, foreign_walls = 0;
    for (const auto &wall : walls) {
        const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall.collection->entities.front());
        REQUIRE(loop != nullptr);
        const bool far_island = loop->first_point().x() >= scale_(30.);
        if (wall.is_shell()) {
            ++native_shells;
            CHECK(loop->inset_idx == 1);
        } else {
            ++foreign_walls;
            // Protect both visible sides of the material if contour and hole
            // walls merge deeper inside. The disconnected island stays separate.
            CHECK(wall.shells.size() == (far_island ? 1 : 2));
            for (const auto *shell : wall.shells) {
                CHECK(shell->get_max_inset_idx() == 1);
                CHECK((shell->first_point().x() >= scale_(30.)) == far_island);
                CHECK(shell != wall.collection);
            }
        }
    }
    CHECK(native_shells == 3);
    CHECK(foreign_walls == 3);
}

TEST_CASE("Merged hole purge walls preserve each hole's native shell", "[PurgeIntoInnerWalls][Regression]")
{
    ExtrusionEntityCollection perimeters;
    perimeters.append(native_shell_test_wall(1, 4., 4., 6., 6., true));
    perimeters.append(native_shell_test_wall(1, 8., 4., 10., 6., true));
    perimeters.append(native_shell_test_wall(2, 3., 3., 11., 7., true));
    perimeters.split_purge_inner_walls();
    const auto walls = purge_test_walls(perimeters);
    REQUIRE(walls.size() == 3);
    CHECK(walls[0].is_shell());
    CHECK(walls[1].is_shell());
    CHECK_FALSE(walls[2].is_shell());
    REQUIRE(walls[2].shells.size() == 2);
    CHECK(walls[2].shells[0] == walls[0].collection);
    CHECK(walls[2].shells[1] == walls[1].collection);
    // The shared deeper wall is counted only once, not once per hole.
    const double foreign_volume = walls[2].collection->total_volume();
    CHECK_THAT(perimeters.total_volume() - walls[0].collection->total_volume() - walls[1].collection->total_volume(),
               Catch::Matchers::WithinAbs(foreign_volume, 1e-6));
}

TEST_CASE("A sole inner wall reserves all of its space for the native shell", "[PurgeIntoInnerWalls][Regression]")
{
    ExtrusionEntityCollection perimeters;
    perimeters.append(native_shell_test_wall(0, 0., 0., 10., 10.));
    perimeters.append(native_shell_test_wall(1, 1., 1., 9., 9.));
    perimeters.split_purge_inner_walls();
    const auto walls = purge_test_walls(perimeters);
    REQUIRE(walls.size() == 1);
    CHECK(walls.front().is_shell());
    CHECK(walls.front().collection->total_volume() > 0.);
    double foreign_capacity = 0.;
    for (const auto &wall : walls)
        if (!wall.is_shell())
            foreign_capacity += wall.collection->total_volume();
    CHECK_THAT(foreign_capacity, Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Native shells follow walls across compatible region collections", "[PurgeIntoInnerWalls][Regression]")
{
    ExtrusionEntityCollection first_region, second_region;
    first_region.append(native_shell_test_wall(1, 0., 0., 20., 20.));
    second_region.append(native_shell_test_wall(2, 1., 1., 19., 19.));
    first_region.split_purge_inner_walls();
    second_region.split_purge_inner_walls();
    const auto *shell = dynamic_cast<const ExtrusionEntityCollection *>(first_region.entities.front());
    const auto *purge = dynamic_cast<const ExtrusionEntityCollection *>(second_region.entities.front());
    REQUIRE(shell != nullptr);
    REQUIRE(purge != nullptr);
    const auto walls = ExtrusionEntityCollection::purge_inner_walls({purge, shell});
    REQUIRE(walls.size() == 2);
    CHECK(walls[0].is_shell());
    CHECK_FALSE(walls[1].is_shell());
    REQUIRE(walls[1].shells.size() == 1);
    CHECK(walls[1].shells.front() == shell);
    CHECK(walls[1].collection == purge);
}


TEST_CASE("Native shell planning rejects unclosed or disconnected purge loops", "[PurgeIntoInnerWalls][Regression]")
{
    const int invalid_geometry = GENERATE(0, 1, 2);
    ExtrusionLoop loop = native_shell_test_wall(1, 0., 0., 20., 20.);
    if (invalid_geometry == 0)
        loop.paths.front().polyline.points.clear();
    else if (invalid_geometry == 1)
        loop.paths.front().polyline.points.pop_back();
    else {
        // The endpoints close, but the middle is missing a segment.
        ExtrusionPath tail = loop.paths.front();
        auto &head_points = loop.paths.front().polyline.points;
        head_points.erase(head_points.begin() + 2, head_points.end());
        tail.polyline.points.erase(tail.polyline.points.begin(), tail.polyline.points.begin() + 2);
        loop.paths.push_back(std::move(tail));
    }
    ExtrusionEntityCollection perimeters;
    perimeters.append(std::move(loop));
    perimeters.split_purge_inner_walls();
    CHECK(purge_test_walls(perimeters).empty());
}

TEST_CASE("Purge shortfall counts follow filament statistics aggregation", "[PurgeIntoInnerWalls][Regression]")
{
    FilamentChangeStats first, second;
    first.filament_flush_weight = 3;
    first.flush_filament_change_count = 4;
    first.filament_change_count = 5;
    first.purge_inner_walls_affected_layer_count = 6;
    first.purge_inner_walls_affected_transition_count = 7;
    second.filament_flush_weight = 10;
    second.flush_filament_change_count = 20;
    second.filament_change_count = 30;
    second.purge_inner_walls_affected_layer_count = 40;
    second.purge_inner_walls_affected_transition_count = 50;

    const auto check_sum = [](const FilamentChangeStats &stats) {
        CHECK(stats.filament_flush_weight == 13);
        CHECK(stats.flush_filament_change_count == 24);
        CHECK(stats.filament_change_count == 35);
        CHECK(stats.purge_inner_walls_affected_layer_count == 46);
        CHECK(stats.purge_inner_walls_affected_transition_count == 57);
    };
    check_sum(first + second);
    CHECK(first.purge_inner_walls_affected_layer_count == 6);
    CHECK(first.purge_inner_walls_affected_transition_count == 7);
    first += second;
    check_sum(first);
    first.clear();
    CHECK(first.filament_flush_weight == 0);
    CHECK(first.flush_filament_change_count == 0);
    CHECK(first.filament_change_count == 0);
    CHECK(first.purge_inner_walls_affected_layer_count == 0);
    CHECK(first.purge_inner_walls_affected_transition_count == 0);
}
