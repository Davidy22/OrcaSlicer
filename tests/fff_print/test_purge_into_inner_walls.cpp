#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "test_helpers.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <cstddef>
#include <vector>

using namespace Slic3r;

namespace {
DynamicPrintConfig purge_config(const char *generator, int walls, bool legacy_flush = false)
{
    return Test::multifilament_config(2, {
        {"wall_generator", generator},
        {"wall_loops", walls},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"top_shell_layers", 1},
        {"bottom_shell_layers", 1},
        {"only_one_wall_first_layer", false},
        {"only_one_wall_top", false},
        {"sparse_infill_density", "15%"},
        {"print_sequence", "by layer"},
        {"brim_type", "no_brim"},
        {"skirt_loops", 0},
        {"enable_prime_tower", false},
        {"purge_into_inner_walls", true},
        {"purge_into_inner_walls_preferred", false},
        {"single_extruder_multi_material", true},
        {"purge_in_prime_tower", false},
        {"prime_volume", 10.}, // use a known demand, independent of the flush matrix
        {"flush_into_objects", legacy_flush},
        {"flush_into_infill", legacy_flush},
        {"flush_into_support", legacy_flush},
    });
}

void init_purge_print(Print &print, Model &model, const DynamicPrintConfig &config, bool prefer_first = false)
{
    // Separated in known world coordinates so emitted outer-wall moves can
    // be matched to their object's intended filament, not just counted.
    TriangleMesh first = make_cube(20., 20., 1.);
    TriangleMesh second = make_cube(20., 20., 1.);
    first.translate(40.f, 40.f, 0.f);
    second.translate(80.f, 40.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", prefer_first}},
        {{"extruder", 2}},
    };
    Test::init_print(std::vector<TriangleMesh>{first, second}, print, model, config, &overrides, false);
}
} // namespace

TEST_CASE("Purge overrides and emitted colors are restricted to inner walls", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const int walls = GENERATE(1, 3);
    const bool legacy_flush = GENERATE(false, true);
    const bool prefer_first = GENERATE(false, true);
    CAPTURE(generator, walls, legacy_flush, prefer_first);

    Model model;
    Print print;
    const DynamicPrintConfig config = purge_config(generator, walls, legacy_flush);
    init_purge_print(print, model, config, prefer_first);
    print.process();
    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects().front()->config().flush_into_objects.getBool() == legacy_flush);

    size_t purge_walls = 0;
    size_t outer_walls = 0;
    size_t generated_walls = 0;
    for (const LayerTools &tools : print.tool_ordering()) {
        // Outer walls must keep both original filaments in the tool order,
        // even if all of the inner walls are available for purge.
        CHECK(tools.has_extruder(0));
        CHECK(tools.has_extruder(1));
        for (const PrintObject *object : print.objects()) {
            const Layer *layer = object->get_layer_at_printz(tools.print_z, EPSILON);
            if (layer == nullptr)
                continue;
            for (const LayerRegion *region : layer->regions()) {
                for (const ExtrusionEntity *entity : region->perimeters.entities) {
                    const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity);
                    REQUIRE(collection != nullptr);
                    const bool overridden = tools.wiping_extrusions().is_entity_overridden(collection, object, 0);
                    if (overridden) {
                        ++purge_walls;
                        CHECK(collection->is_purge_inner_wall_candidate());
                    }
                    for (const ExtrusionEntity *wall : collection->entities) {
                        if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall)) {
                            if (loop->generated_for_purge)
                                ++generated_walls;
                            if (loop->inset_idx == 0) {
                                ++outer_walls;
                                CHECK_FALSE(overridden);
                            }
                        }
                        for_each_extrusion_path(*wall, [overridden](const ExtrusionPath &path) {
                            if (path.role() == erExternalPerimeter)
                                CHECK_FALSE(overridden);
                        });
                    }
                }
                // Stale legacy destinations cannot consume any purge.
                for (const ExtrusionEntity *fill : region->fills.entities)
                    CHECK_FALSE(tools.wiping_extrusions().is_entity_overridden(fill, object, 0));
            }
        }
    }
    REQUIRE(outer_walls > 0);
    REQUIRE(purge_walls > 0); // cannot pass by disabling the feature entirely
    if (walls == 1)
        REQUIRE(generated_walls > 0);

    // Verify the actual export too: protecting allocator metadata alone is
    // insufficient if GCode::process_layer later reassigns a normal wall.
    GCodeProcessorResult result;
    Test::gcode(print, &result);
    std::vector<size_t> emitted_outer_moves(print.objects().size(), 0);
    for (const auto &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter)
            continue;
        size_t matches = 0;
        for (size_t i = 0; i < print.objects().size(); ++i) {
            const PrintObject *object = print.objects()[i];
            const BoundingBoxf3 box = object->instances().front().get_bounding_box();
            const bool in_xy = move.position.x() >= box.min.x() - 0.05 && move.position.x() <= box.max.x() + 0.05 &&
                               move.position.y() >= box.min.y() - 0.05 && move.position.y() <= box.max.y() + 0.05;
            if (in_xy) {
                ++matches;
                ++emitted_outer_moves[i];
                const int expected_filament = object->model_object()->config.opt_int("extruder") - 1;
                CHECK(int(move.extruder_id) == expected_filament);
            }
        }
        CHECK(matches == 1);
    }
    for (const size_t count : emitted_outer_moves)
        REQUIRE(count > 0);
}

TEST_CASE("Changing a purge preference invalidates every object's walls", "[PurgeIntoInnerWalls][Regression]")
{
    Model model;
    Print print;
    const DynamicPrintConfig config = purge_config("arachne", 1);
    init_purge_print(print, model, config);
    print.process();
    for (const PrintObject *object : print.objects())
        REQUIRE(object->is_step_done(posPerimeters));

    model.objects.front()->config.set_key_value("purge_into_inner_walls_preferred", new ConfigOptionBool(true));
    print.apply(model, config);
    for (const PrintObject *object : print.objects())
        CHECK_FALSE(object->is_step_done(posPerimeters));
    CHECK_FALSE(print.is_step_done(psWipeTower));
}
