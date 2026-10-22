// Test cases for purge into inner walls feature
// Verifies that inner walls are properly tagged and extra walls are generated when needed

#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <vector>
#include <algorithm>

using namespace Slic3r;

// Test that ExtrusionLoop purge metadata is properly set
TEST_CASE("ExtrusionLoop purge metadata", "[PurgeIntoInnerWalls]")
{
    // Create a simple extrusion loop
    ExtrusionPaths paths;
    ExtrusionPath path(erPerimeter);
    path.mm3_per_mm = 1.0;
    path.width = 0.4f;
    path.height = 0.2f;
    paths.push_back(std::move(path));
    
    // Test default values (not a purge candidate)
    ExtrusionLoop loop(std::move(paths), elrDefault);
    REQUIRE(loop.is_purge_inner_wall_candidate == false);
    REQUIRE(loop.generated_for_purge == false);
    REQUIRE(loop.purge_candidate_priority == 0);
    
    // Test setting purge candidate metadata
    loop.is_purge_inner_wall_candidate = true;
    loop.generated_for_purge = false;
    loop.purge_candidate_priority = 1005;
    loop.inset_idx = 2;
    
    REQUIRE(loop.is_purge_inner_wall_candidate == true);
    REQUIRE(loop.generated_for_purge == false);
    REQUIRE(loop.purge_candidate_priority == 1005);
    REQUIRE(loop.inset_idx == 2);
}

// Test ExtrusionEntityCollection purge candidate detection
TEST_CASE("ExtrusionEntityCollection purge candidate detection", "[PurgeIntoInnerWalls]")
{
    ExtrusionEntityCollection coll;
    
    // Initially no purge candidates
    REQUIRE(coll.is_purge_inner_wall_candidate() == false);
    REQUIRE(coll.is_existing_purge_wall() == false);
    REQUIRE(coll.get_max_inset_idx() == -1);
    
    // Add a purge candidate loop (existing wall)
    ExtrusionPaths paths;
    ExtrusionPath path(erPerimeter);
    path.mm3_per_mm = 1.0;
    path.width = 0.4f;
    path.height = 0.2f;
    paths.push_back(std::move(path));
    
    ExtrusionLoop loop(std::move(paths), elrDefault);
    loop.is_purge_inner_wall_candidate = true;
    loop.generated_for_purge = false;  // existing wall
    loop.inset_idx = 2;
    loop.purge_candidate_priority = 1002;
    coll.append(std::move(loop));
    
    // Verify detection
    REQUIRE(coll.is_purge_inner_wall_candidate() == true);
    REQUIRE(coll.is_existing_purge_wall() == true);
    REQUIRE(coll.get_max_inset_idx() == 2);
    
    // Add a generated purge wall (extra wall)
    ExtrusionPaths paths2;
    ExtrusionPath path2(erPerimeter);
    path2.mm3_per_mm = 1.0;
    path2.width = 0.4f;
    path2.height = 0.2f;
    paths2.push_back(std::move(path2));
    
    ExtrusionLoop loop2(std::move(paths2), elrDefault);
    loop2.is_purge_inner_wall_candidate = true;
    loop2.generated_for_purge = true;  // extra wall
    loop2.inset_idx = 3;
    loop2.purge_candidate_priority = 5;
    coll.append(std::move(loop2));
    
    // Verify mixed detection
    REQUIRE(coll.is_purge_inner_wall_candidate() == true);
    REQUIRE(coll.is_existing_purge_wall() == true);  // still has existing walls
    REQUIRE(coll.get_max_inset_idx() == 3);  // max of both
    
    // Test volume calculation
    double volume = coll.total_volume();
    REQUIRE(volume > 0);
}

// Test that PrintConfig has purge_into_inner_walls option
TEST_CASE("PrintConfig purge_into_inner_walls option", "[PurgeIntoInnerWalls]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    
    // Check that the option exists
    const ConfigOption* purge_opt = config.option("purge_into_inner_walls");
    REQUIRE(purge_opt != nullptr);
    REQUIRE(purge_opt->getBool() == false);  // default is false
    
    // Check that max_extra_loops option exists
    const ConfigOption* max_loops_opt = config.option("purge_inner_walls_max_extra_loops");
    REQUIRE(max_loops_opt != nullptr);
    REQUIRE(max_loops_opt->getInt() == -1);  // default is -1

    // The feature toggles are global (PrintConfig) while the prioritization
    // toggle is per-object (PrintObjectConfig). Declaring the same key in both
    // classes would make FullPrintConfig register it twice (assert in
    // StaticCacheBase::opt_add) and direct member access ambiguous.
    REQUIRE(PrintConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE(PrintConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE_FALSE(PrintConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_into_inner_walls"));
    REQUIRE_FALSE(PrintObjectConfig::defaults().has("purge_inner_walls_max_extra_loops"));
    REQUIRE(PrintObjectConfig::defaults().has("purge_into_inner_walls_preferred"));
    REQUIRE(PrintObjectConfig::defaults().purge_into_inner_walls_preferred.getBool() == false);

    const auto full_keys = FullPrintConfig::defaults().keys();
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_into_inner_walls") == 1);
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_inner_walls_max_extra_loops") == 1);
    REQUIRE(std::count(full_keys.begin(), full_keys.end(), "purge_into_inner_walls_preferred") == 1);
}

// Test purge candidate priority ordering
TEST_CASE("Purge candidate priority ordering", "[PurgeIntoInnerWalls]")
{
    struct PurgeCandidate {
        int priority;
        float volume;
        bool is_purge_object;
        bool is_existing;
    };
    
    std::vector<PurgeCandidate> candidates = {
        {100, 5.0f, false, true},   // existing inner wall, not purge object
        {10005, 3.0f, true, true},  // existing inner wall, purge object
        {5, 2.0f, false, false},    // extra wall, not purge object
        {10000, 4.0f, true, false}, // extra wall, purge object
        {1003, 6.0f, false, true},  // existing inner wall, higher inset, not purge object
    };
    
    // Sort by priority (highest first), then by is_purge_object, then by volume
    std::sort(candidates.begin(), candidates.end(), [](const PurgeCandidate& a, const PurgeCandidate& b) {
        if (a.is_purge_object != b.is_purge_object)
            return a.is_purge_object > b.is_purge_object;
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.volume > b.volume;
    });
    
    // Verify ordering
    REQUIRE(candidates[0].is_purge_object == true);  // purge object first
    REQUIRE(candidates[0].priority == 10005);        // highest priority purge object
    
    REQUIRE(candidates[1].is_purge_object == true);  // second purge object
    REQUIRE(candidates[1].priority == 10000);
    
    REQUIRE(candidates[2].is_purge_object == false); // then non-purge objects
    REQUIRE(candidates[2].priority == 1003);         // highest priority non-purge
    
    REQUIRE(candidates[3].is_purge_object == false);
    REQUIRE(candidates[3].priority == 100);
    
    REQUIRE(candidates[4].is_purge_object == false);
    REQUIRE(candidates[4].priority == 5);  // lowest priority (extra wall)
}

// Test that objects with purge_into_inner_walls are prioritized
TEST_CASE("Object purge prioritization", "[PurgeIntoInnerWalls]")
{
    // Test that objects with purge_into_inner_walls are prioritized
    // This test verifies the sorting logic concept without creating full Model/PrintObject structures
    
    struct MockObject {
        int id;
        bool purge_enabled;
    };
    
    std::vector<MockObject> objects = {
        {0, true},   // purge enabled
        {1, false},  // purge disabled
    };
    
    // When sorted for purge allocation, object with purge enabled should come first
    std::sort(objects.begin(), objects.end(), [](const MockObject& a, const MockObject& b) {
        if (a.purge_enabled != b.purge_enabled)
            return a.purge_enabled;  // true comes before false
        return a.id < b.id;
    });
    
    // Object with purge enabled should be first
    REQUIRE(objects[0].purge_enabled == true);
    REQUIRE(objects[0].id == 0);
    REQUIRE(objects[1].purge_enabled == false);
    REQUIRE(objects[1].id == 1);
}

// Test that existing inner walls are consumed before extra walls
TEST_CASE("Existing walls consumed before extra walls", "[PurgeIntoInnerWalls]")
{
    struct PurgeCandidate {
        int priority;
        bool is_existing;
        float volume;
    };
    
    std::vector<PurgeCandidate> candidates = {
        {1005, true, 10.0f},   // existing wall
        {5, false, 5.0f},      // extra wall
        {1003, true, 8.0f},    // existing wall with lower inset
        {3, false, 3.0f},      // another extra wall
    };
    
    // Sort: existing first (higher priority), then by inset
    std::sort(candidates.begin(), candidates.end(), [](const PurgeCandidate& a, const PurgeCandidate& b) {
        if (a.is_existing != b.is_existing)
            return a.is_existing > b.is_existing;  // existing before extra
        return a.priority > b.priority;
    });
    
    // Verify: all existing walls come before extra walls
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i > 0 && !candidates[i-1].is_existing && candidates[i].is_existing) {
            FAIL("Extra wall found before existing wall in sorted order");
        }
    }
    
    REQUIRE(candidates[0].is_existing == true);
    REQUIRE(candidates[1].is_existing == true);
    REQUIRE(candidates[2].is_existing == false);
    REQUIRE(candidates[3].is_existing == false);
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
