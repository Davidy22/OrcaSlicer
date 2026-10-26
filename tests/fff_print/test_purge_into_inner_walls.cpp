#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {
DynamicPrintConfig purge_config(const char *generator, int walls, bool legacy_flush = false, unsigned int filaments = 2)
{
    return Test::multifilament_config(filaments, {
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

void init_purge_print(Print &print, Model &model, const DynamicPrintConfig &config, bool prefer_first = false, size_t copies = 1)
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
    Test::init_print(std::vector<TriangleMesh>{first, second}, print, model, config, &overrides, false, copies);
    // Keep copies physically separate without changing their orientation (and
    // therefore without splitting them into different PrintObjects).
    if (copies > 1) {
        for (ModelObject *object : model.objects)
            for (size_t copy = 1; copy < copies; ++copy)
                object->instances[copy]->set_offset(object->instances[copy]->get_offset() + Vec3d(0., 30. * copy, 0.));
        print.apply(model, config);
    }
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

TEST_CASE("Enabling inner-wall purge does not emit a blanket validation warning", "[PurgeIntoInnerWalls][Regression]")
{
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config("classic", 3);
    config.set_deserialize_strict({
        {"use_relative_e_distances", true},
        {"layer_change_gcode", "G92 E0\n"},
    });
    init_purge_print(print, model, config);
    REQUIRE(print.objects().size() == 2);
    std::vector<StringObjectException> warnings;
    const StringObjectException validation = print.validate(&warnings);
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    for (const StringObjectException& warning : warnings)
        CHECK(warning.opt_key != "purge_into_inner_walls");
    // A measured capacity shortfall is still reported by the purge planner
    // after slicing; opting into the mode alone must not produce a warning.
}

TEST_CASE("Matching purge filament occupies the walls nearest the exterior", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool matching_first = GENERATE(false, true);
    const int inner_filament = GENERATE(0, 2); // default, or deliberately different from the exterior
    const size_t copies = GENERATE(size_t{1}, size_t{2});
    CAPTURE(generator, matching_first, inner_filament, copies);

    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 6, false, 3);
    config.set_deserialize_strict({{"prime_volume", 1.}, {"inner_wall_filament_id", inner_filament}});
    init_purge_print(print, model, config, true, copies);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *preferred = print.objects().front();
    REQUIRE(preferred->instances().size() == copies);
    REQUIRE(preferred->layers().size() > 1);
    const Layer *layer = preferred->layers()[1];

    // Exercise the real allocator on sliced Classic/Arachne geometry in both
    // transition orders. Do not depend on the color-distance optimizer choosing
    // a particular first tool, and do not swap tools after budgets are charged.
    LayerTools tools(layer->print_z);
    tools.extruders = matching_first ? std::vector<unsigned int>{0, 1, 2} : std::vector<unsigned int>{1, 2, 0};
    WipingExtrusions &wiping = tools.wiping_extrusions();
    double closest_volume = 0.;
    int closest_inset = std::numeric_limits<int>::max();
    for (const PrintObject *object : print.objects()) {
        const Layer *object_layer = object->get_layer_at_printz(tools.print_z, EPSILON);
        REQUIRE(object_layer != nullptr);
        for (const LayerRegion *region : object_layer->regions())
            for (const ExtrusionEntity *entity : region->perimeters.entities) {
                const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(entity);
                REQUIRE(wall != nullptr);
                const bool eligible = wiping.is_overriddable_and_mark(*wall, print.config(), *object, region->region());
                if (object == preferred && eligible && wall->get_max_inset_idx() < closest_inset) {
                    closest_inset = wall->get_max_inset_idx();
                    closest_volume = wall->total_volume();
                }
            }
    }
    REQUIRE(closest_volume > 0.);
    REQUIRE(closest_inset > 0);
    // Enough to claim one wall per copy, but not all of this object's walls.
    const float demand = float(0.75 * closest_volume * copies);
    FilamentChangeStats stats;
    unsigned int previous = matching_first ? 2 : 0;
    for (const unsigned int filament : tools.extruders) {
        CHECK_THAT(wiping.mark_wiping_extrusions(print, previous, filament, demand, stats),
                   Catch::Matchers::WithinAbs(0., 1e-5));
        previous = filament;
    }

    for (size_t copy = 0; copy < copies; ++copy) {
        int furthest_matching = -1;
        int nearest_foreign = std::numeric_limits<int>::max();
        std::vector<bool> colors(3, false);
        for (const LayerRegion *region : layer->regions()) {
            REQUIRE(tools.wall_extruder_id(region->region()) == 0);
            for (const ExtrusionEntity *entity : region->perimeters.entities) {
                const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(entity);
                REQUIRE(wall != nullptr);
                const int filament = wiping.get_extruder_override(wall, preferred, copy);
                if (!wall->is_purge_inner_wall_candidate()) {
                    CHECK(filament < 0); // exterior/hole-boundary groups remain protected
                    continue;
                }
                if (filament < 0)
                    continue;
                REQUIRE(filament < int(colors.size()));
                colors[filament] = true;
                if (filament == int(tools.wall_extruder_id(region->region())))
                    furthest_matching = std::max(furthest_matching, wall->get_max_inset_idx());
                else
                    nearest_foreign = std::min(nearest_foreign, wall->get_max_inset_idx());
            }
        }
        for (const bool present : colors)
            REQUIRE(present);
        CHECK(furthest_matching == closest_inset);
        CHECK(furthest_matching < nearest_foreign);
    }
    // The per-object preference still wins over a nonpreferred color match.
    const PrintObject *other = print.objects()[1];
    const Layer *other_layer = other->get_layer_at_printz(tools.print_z, EPSILON);
    REQUIRE(other_layer != nullptr);
    for (const LayerRegion *region : other_layer->regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            for (size_t copy = 0; copy < copies; ++copy)
                CHECK(wiping.get_extruder_override(entity, other, copy) < 0);
}

TEST_CASE("Only purge-designated inner walls use purge speeds in exported G-code", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool enabled = GENERATE(false, true);
    const double flush_flow = GENERATE(0., 1.);
    const double infill_speed = GENERATE(40., 150.);
    CAPTURE(generator, enabled, flush_flow, infill_speed);
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 6);
    config.set_deserialize_strict({
        {"purge_into_inner_walls", enabled},
        {"inner_wall_speed", 180.},
        {"outer_wall_speed", 80.},
        {"is_infill_first", true}, // also exercise the post-infill perimeter emission pass
        {"initial_layer_speed", 30.},
        {"sparse_infill_speed", infill_speed},
        {"wipe_tower_max_purge_speed", 60.},
        {"filament_max_volumetric_speed", "50,50"},
        {"filament_flow_ratio", "1,1"},
        {"filament_flush_volumetric_speed", std::to_string(flush_flow) + "," + std::to_string(flush_flow)},
        {"slow_down_for_layer_cooling", false},
        {"slow_down_layers", 0},
        {"small_perimeter_threshold", 0.},
        {"enable_overhang_speed", false},
        {"enable_arc_fitting", false},
        {"wipe_inward", false},
        {"use_relative_e_distances", false},
        {"change_filament_gcode", ";TEST_PURGE_START [next_extruder]\n"},
    });
    init_purge_print(print, model, config, true);
    const std::string gcode = Test::gcode(print);

    bool purging = false;
    bool first_purge_wall = false;
    int filament = -1;
    std::string role;
    size_t purge_moves = 0, matching_purge_moves = 0, flow_checks = 0;
    size_t fast_native_inner_moves = 0, fast_outer_moves = 0, purge_starts = 0;
    size_t first_layer_purge_starts = 0;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string comment(line.comment());
        if (comment.rfind("TEST_PURGE_START ", 0) == 0) {
            std::istringstream args(comment.substr(std::string("TEST_PURGE_START ").size()));
            args >> filament;
            purging = true;
            first_purge_wall = true;
        } else if (comment.find("PURGING FINISHED") != std::string::npos)
            purging = false;
        else if (comment.rfind("TYPE:", 0) == 0)
            role = comment.substr(5);
        if (!line.extruding(state) || line.dist_XY(state) <= 1e-5)
            return;
        const double speed = line.new_F(state) / Test::MM_PER_MIN;
        if (enabled && purging && role == "Inner wall") {
            ++purge_moves;
            const bool first_layer = line.new_Z(state) <= 0.2 + 1e-4;
            const double target = first_layer ? std::min({60., infill_speed, 30.}) : std::min(60., infill_speed);
            CHECK(speed <= target + 1e-3);
            if (first_purge_wall) {
                ++purge_starts;
                if (first_layer)
                    ++first_layer_purge_starts;
                CHECK(speed <= 0.33 * target + 1e-3);
                first_purge_wall = false;
            }
            // Native-color overrides are purge too; a tool-mismatch test in
            // GCode::_extrude would incorrectly leave these at wall speed.
            for (const PrintObject *object : print.objects()) {
                const BoundingBoxf3 box = object->instances().front().get_bounding_box();
                if (filament == object->model_object()->config.opt_int("extruder") - 1 &&
                    line.new_X(state) >= box.min.x() && line.new_X(state) <= box.max.x() &&
                    line.new_Y(state) >= box.min.y() && line.new_Y(state) <= box.max.y())
                    ++matching_purge_moves;
            }
            if (flush_flow > 0. && line.dist_XY(state) > 0.5) {
                const double volume = line.dist_E(state) * PI * (1.75 * 1.75 / 4.);
                CHECK(speed * volume / line.dist_XY(state) <= flush_flow + 0.01);
                ++flow_checks;
            }
        } else if (role == "Inner wall" && speed > 60.)
            ++fast_native_inner_moves;
        if (role == "Outer wall" && speed > 60.)
            ++fast_outer_moves;
    });
    REQUIRE(fast_native_inner_moves > 0);
    REQUIRE(fast_outer_moves > 0);
    if (enabled) {
        REQUIRE(purge_moves > 0);
        REQUIRE(first_layer_purge_starts > 0);
        REQUIRE(purge_starts > first_layer_purge_starts);
        REQUIRE(matching_purge_moves > 0);
        if (flush_flow > 0.)
            REQUIRE(flow_checks > 0);
    } else
        CHECK(purge_moves == 0);
}
