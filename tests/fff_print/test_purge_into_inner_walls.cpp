#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"
#include "test_utils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PurgeIntoInnerWalls.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

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

TEST_CASE("Both perimeter generators preserve native depths independently of purge tagging", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool enabled = GENERATE(false, true);
    CAPTURE(generator, enabled);
    Model model;
    Print print;
    auto config = purge_config(generator, 4, false, 3);
    config.set_deserialize_strict({{"purge_into_inner_walls", enabled}, {"prime_volume", 0.}});
    Test::init_print({make_cube(20., 20., 1.)}, print, model, config);
    print.process();
    // Configured-but-unused slots must not turn this into a purge job.
    REQUIRE(print.tool_ordering().all_extruders().size() == 1);
    for (const LayerTools &tools : print.tool_ordering())
        CHECK_FALSE(tools.wiping_extrusions().is_anything_overridden());
    std::set<int> depths;
    const int native_path_depth = ExtrusionPath(erPerimeter).inset_idx;
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) {
                    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                        REQUIRE(loop->inset_idx >= 0);
                        depths.insert(loop->inset_idx);
                        const bool candidate = enabled && loop->inset_idx > 0;
                        CHECK(loop->is_purge_inner_wall_candidate == candidate);
                        CHECK_FALSE(loop->generated_for_purge);
                        for (const ExtrusionPath &path : loop->paths)
                            CHECK(path.inset_idx == native_path_depth);
                    }
                });
    CHECK(depths.count(0) == 1);
    CHECK(depths.count(1) == 1);
}

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
    REQUIRE(print.tool_ordering().all_extruders().size() == 2);
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

TEST_CASE("Purge demand and target changes invalidate model walls only where needed", "[PurgeIntoInnerWalls][Regression]")
{
    const std::string key = GENERATE("purge_into_inner_walls_preferred", "prime_volume", "flush_volumes_matrix",
        "flush_multiplier", "purge_in_prime_tower", "single_extruder_multi_material", "print_sequence",
        "filament_soluble", "filament_is_support", "sparse_infill_filament_id", "internal_solid_filament_id",
        "top_surface_filament_id", "bottom_surface_filament_id");
    const bool enabled = GENERATE(false, true);
    CAPTURE(key, enabled);
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config("arachne", 1);
    config.set_deserialize_strict({{"purge_into_inner_walls", enabled}});
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = {0., 10., 10., 0.};
    config.option<ConfigOptionFloats>("flush_multiplier")->values = {1.};
    init_purge_print(print, model, config);
    print.process();
    for (const PrintObject *object : print.objects())
        REQUIRE(object->is_step_done(posPerimeters));

    if (key == "purge_into_inner_walls_preferred")
        model.objects.front()->config.set_key_value(key, new ConfigOptionBool(true));
    else if (key == "prime_volume")
        config.set_deserialize_strict({{key, 20.}});
    else if (key == "flush_volumes_matrix")
        config.option<ConfigOptionFloats>(key)->values = {0., 20., 20., 0.};
    else if (key == "flush_multiplier")
        config.option<ConfigOptionFloats>(key)->values = {2.};
    else if (key == "print_sequence")
        config.set_deserialize_strict(key, "by object");
    else if (key == "filament_soluble" || key == "filament_is_support")
        config.option<ConfigOptionBools>(key)->values = {false, true};
    else if (key.find("_filament_id") != std::string::npos)
        // Only one object's infill changes, but every donor's demand can change.
        model.objects.front()->config.set_key_value(key, new ConfigOptionInt(2));
    else
        config.set_deserialize_strict({{key, !config.option<ConfigOptionBool>(key)->value}});
    print.apply(model, config);
    const bool regenerate_walls = enabled || key == "purge_into_inner_walls_preferred";
    for (const PrintObject *object : print.objects())
        CHECK(object->is_step_done(posPerimeters) == !regenerate_walls);
    CHECK_FALSE(print.is_step_done(psWipeTower));
}

TEST_CASE("Incremental purge wall layouts match fresh prints after shared demand changes", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool enabled = GENERATE(false, true);
    const std::string change = GENERATE("add copy", "remove copy", "source color", "remove source",
                                       "remove preferred", "export only", "move copy");
    CAPTURE(generator, enabled, change);
    Model model;
    Print print;
    auto config = purge_config(generator, 1, false, 3);
    config.set_deserialize_strict({{"purge_into_inner_walls", enabled}, {"prime_volume", 18.}});
    init_purge_print(print, model, config, change != "remove preferred", change == "remove copy" ? 2 : 1);
    if (change == "remove preferred") {
        // Removing the only preferred target must activate growth on both
        // survivors, not leave their cached single-wall geometry unchanged.
        ModelObject *preferred = model.add_object(*model.objects.back());
        preferred->config.set_key_value("extruder", new ConfigOptionInt(3));
        preferred->config.set_key_value("purge_into_inner_walls_preferred", new ConfigOptionBool(true));
        preferred->instances.front()->set_offset(preferred->instances.front()->get_offset() + Vec3d(0., 30., 0.));
        print.apply(model, config);
    }
    print.process();
    const auto perimeter_timestamp = print.objects().front()->step_state_with_timestamp(posPerimeters).timestamp;
    if (change == "add copy") {
        ModelObject *target = model.objects.front();
        ModelInstance *copy = target->add_instance(*target->instances.front());
        copy->set_offset(copy->get_offset() + Vec3d(0., 30., 0.));
    } else if (change == "remove copy")
        model.objects.front()->delete_instance(1);
    else if (change == "source color")
        model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(1));
    else if (change == "remove source" || change == "remove preferred")
        model.delete_object(model.objects.size() - 1);
    else if (change == "export only")
        config.option<ConfigOptionStrings>("filament_notes")->values.front() = "export-only change";
    else {
        ModelInstance *copy = model.objects.front()->instances.front();
        copy->set_offset(copy->get_offset() + Vec3d(0., 30., 0.));
    }
    print.apply(model, config);
    const bool changes_demand = change != "export only" && change != "move copy";
    const bool regenerate_walls = enabled && changes_demand;
    // This donor is untouched when a different source changes or disappears.
    CHECK(print.objects().front()->is_step_done(posPerimeters) == !regenerate_walls);
    print.process();
    if (!changes_demand)
        CHECK(print.objects().front()->step_state_with_timestamp(posPerimeters).timestamp == perimeter_timestamp);

    Print fresh;
    fresh.apply(model, config);
    fresh.process();
    const auto layout = [](const Print &active) {
        std::vector<std::pair<size_t, double>> result;
        for (const PrintObject *object : active.objects())
            for (const Layer *layer : object->layers()) {
                size_t loops = 0;
                double volume = 0.;
                for (const LayerRegion *region : layer->regions()) {
                    volume += region->perimeters.total_volume();
                    for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) { loops += entity.is_loop(); });
                }
                result.emplace_back(loops, volume);
            }
        return result;
    };
    const auto incremental = layout(print), expected = layout(fresh);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(incremental.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(incremental[i].first == expected[i].first);
        CHECK_THAT(incremental[i].second, Catch::Matchers::WithinAbs(expected[i].second, 1e-5));
    }
}

TEST_CASE("Blocked material transitions do not grow unused model walls", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const std::string material = GENERATE("filament_soluble", "filament_is_support");
    const size_t protected_filament = GENERATE(size_t{0}, size_t{1});
    CAPTURE(generator, material, protected_filament);
    Model model;
    Print print;
    auto config = purge_config(generator, 1);
    config.set_deserialize_strict({{"prime_volume", 1000.}});
    init_purge_print(print, model, config);
    print.process();
    const auto grown_loops = [](const Print &active) {
        size_t count = 0;
        for (const PrintObject *object : active.objects())
            for (const Layer *layer : object->layers())
                for (const LayerRegion *region : layer->regions())
                    for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) {
                        if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
                            count += loop->generated_for_purge;
                    });
        return count;
    };
    REQUIRE(grown_loops(print) > 0); // prove the unprotected control needs growth
    config.option<ConfigOptionBools>(material)->values[protected_filament] = true;
    print.apply(model, config);
    print.process();
    // Both directions now involve protected material: neither has usable demand.
    CHECK(grown_loops(print) == 0);
    for (const LayerTools &tools : print.tool_ordering())
        CHECK_FALSE(tools.wiping_extrusions().is_anything_overridden());
}

TEST_CASE("Purge wall replanning restores changed inputs and preserves unrelated work", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool complete = GENERATE(false, true);
    const std::string change = GENERATE("demand", "preference", "disable", "unchanged", "export", "copy source");
    const bool reuse_perimeters = change == "unchanged" || change == "export";
    const bool stop_after_first = reuse_perimeters || change == "copy source";
    CAPTURE(generator, complete, change);
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 1);
    config.set_deserialize_strict({{"prime_volume", 1000.}});
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"probe"}));
    init_purge_print(print, model, config);
    if (complete)
        print.process();
    else {
        // Stop before infill types the slices. Keep the second object's walls
        // pending for edits that must either reuse the first object's work or
        // stop readers of the second object's still-invalid inputs.
        {
            Print::set_slicing_pipeline_hook_fn([&](Print &active, const PrintObject *object, SlicingPipelineStepPlugin step) {
                const PrintObject *last = stop_after_first ? print.objects().front() : print.objects().back();
                if (step == SlicingPipelineStepPlugin::posPerimeters && object == last)
                    active.cancel();
            });
            const ScopeGuard reset_hook([] { Print::set_slicing_pipeline_hook_fn(nullptr); });
            REQUIRE_THROWS_AS(print.process(), CanceledException);
        }
        for (const PrintObject *object : print.objects()) {
            const bool done = !stop_after_first || object == print.objects().front();
            REQUIRE(object->is_step_done(posPerimeters) == done);
            REQUIRE_FALSE(object->is_step_done(posPrepareInfill));
        }
        print.restart();
    }
    size_t generated = 0;
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) {
                    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
                        generated += loop->generated_for_purge;
                });
    REQUIRE(generated > 0);

    const auto perimeter_timestamp = print.objects().front()->step_state_with_timestamp(posPerimeters).timestamp;
    if (change == "preference")
        model.objects.back()->config.set_key_value("purge_into_inner_walls_preferred", new ConfigOptionBool(true));
    else if (change == "disable")
        config.set_deserialize_strict({{"purge_into_inner_walls", false}});
    else if (change == "demand")
        config.set_deserialize_strict({{"prime_volume", 0.}});
    else if (change == "export")
        config.option<ConfigOptionStrings>("filament_notes")->values.front() = "export-only change";
    if (change == "copy source") {
        ModelObject *source = model.objects.back();
        ModelInstance *copy = source->add_instance(*source->instances.front());
        copy->set_offset(copy->get_offset() + Vec3d(0., 30., 0.));
        size_t copies_at_first_stop = 0;
        print.set_cancel_callback([&] {
            if (copies_at_first_stop == 0)
                copies_at_first_stop = print.objects().back()->instances().size();
        });
        const ScopeGuard reset_callback([&] { print.set_cancel_callback([] {}); });
        print.apply(model, config);
        // With an incomplete pass the source's own steps are already invalid.
        // A cached/active donor still needs stopping BEFORE its inputs change.
        CHECK(copies_at_first_stop == 1);
    } else
        print.apply(model, config);
    if (reuse_perimeters)
        CHECK(print.objects().front()->is_step_done(posPerimeters));
    print.process();
    if (reuse_perimeters)
        CHECK(print.objects().front()->step_state_with_timestamp(posPerimeters).timestamp == perimeter_timestamp);
    for (const PrintObject *object : print.objects()) {
        // Only preference changes restrict growth to one object. Removing
        // demand or disabling purge clears growth altogether; irrelevant edits
        // preserve completed walls rather than regenerating either object.
        const bool should_grow = reuse_perimeters || change == "copy source" ||
            (change == "preference" && object->config().purge_into_inner_walls_preferred.getBool());
        size_t loops = 0, grown = 0;
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) {
                    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                        ++loops;
                        grown += loop->generated_for_purge;
                        if (!should_grow) {
                            CHECK(loop->inset_idx == 0); // the configured single native wall
                            CHECK_FALSE(loop->generated_for_purge);
                        }
                    }
                });
        REQUIRE(loops > 0);
        if (should_grow)
            REQUIRE(grown > 0);
    }
}

TEST_CASE("Shared model copies size purge walls like print instances", "[PurgeIntoInnerWalls][Regression]")
{
    std::vector<size_t> instance_loops;
    for (const bool shared : {false, true}) {
        CAPTURE(shared);
        Model model;
        Print print;
        DynamicPrintConfig config = purge_config("classic", 3);
        config.set_deserialize_strict({{"prime_volume", 18.}, {"line_width", 0.45},
            {"outer_wall_line_width", 0.45}, {"inner_wall_line_width", 0.45}, {"initial_layer_line_width", 0.45}});
        init_purge_print(print, model, config);
        ModelObject *first = model.objects.front();
        ModelInstance *copy = shared ? model.add_object(*first)->instances.front() : first->add_instance(*first->instances.front());
        copy->set_offset(copy->get_offset() + Vec3d(0., 30., 0.));
        print.apply(model, config);
        print.process();
        if (shared)
            REQUIRE(print.objects().back()->get_shared_object() == print.objects().front());
        const PrintObject *source = print.objects().front();
        std::vector<size_t> loops;
        for (const Layer *layer : source->layers()) {
            size_t count = 0;
            for (const LayerRegion *region : layer->regions())
                for_each_extrusion_leaf(region->perimeters, [&](const ExtrusionEntity &entity) {
                    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                        ++count;
                        // The two copies' native inner walls already fit this
                        // demand; sharing must not manufacture extra walls.
                        CHECK_FALSE(loop->generated_for_purge);
                    }
                });
            REQUIRE(count > 0);
            loops.push_back(count);
        }
        REQUIRE_FALSE(loops.empty());
        if (shared)
            CHECK(loops == instance_loops);
        else
            instance_loops = std::move(loops);
    }
}

TEST_CASE("Inner-wall purge rejects only active competing belt purge planners", "[PurgeIntoInnerWalls][Regression]")
{
    const bool belt = GENERATE(false, true);
    const bool tower = GENERATE(false, true);
    const bool purge = GENERATE(false, true);
    const bool prism = GENERATE(false, true);
    CAPTURE(belt, tower, purge, prism);
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config("classic", 3);
    // Relative-E Marlin requires explicit G92 E0, else validate() fails.
    config.set_deserialize_strict({{"belt_printer", belt}, {"enable_belt_purge_tower", tower}, {"purge_into_inner_walls", purge},
                                   {"layer_change_gcode", "G92 E0\n"}});
    init_purge_print(print, model, config);
    model.objects.front()->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(prism));
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    const bool conflict = belt && tower && purge && prism;
    INFO(validation.string);
    if (conflict) {
        CHECK_FALSE(validation.string.empty());
        CHECK(validation.opt_key == "purge_into_inner_walls");
    } else
        CHECK(validation.string.empty());
}

TEST_CASE("Purge demand walks a layer's toolchanges once", "[PurgeIntoInnerWalls]")
{
    // The shared walk behind every purge capacity estimate: model wall growth,
    // support wall growth, proportional reduction and allocation marking.
    std::vector<std::pair<unsigned int, unsigned int>> seen;
    const auto collect = [&seen](unsigned int from, unsigned int to) { seen.emplace_back(from, to); };

    // An unknown entry filament has nothing to change from, so the first
    // filament is not a toolchange; consecutive repeats are not either.
    int previous = -1;
    PurgeIntoInnerWalls::for_each_toolchange(std::vector<unsigned int>{1, 2, 2, 1}, previous, collect);
    CHECK(previous == 1);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == std::make_pair(1u, 2u));
    CHECK(seen[1] == std::make_pair(2u, 1u));

    // The walk carries the last filament across calls, like the objects of one
    // layer or the layers of a print, and an empty sequence changes nothing.
    seen.clear();
    PurgeIntoInnerWalls::for_each_toolchange(std::vector<unsigned int>{1, 3}, previous, collect);
    PurgeIntoInnerWalls::for_each_toolchange(std::vector<unsigned int>{}, previous, collect);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == std::make_pair(1u, 3u));
    CHECK(previous == 3);

    // Summed demand skips pairs the configuration forbids, and leaves the
    // running filament where the caller expects it.
    const auto volume_at = [](unsigned int from, unsigned int to) {
        return (from == 2 && to == 3) ? 0.f : 10.f;
    };
    int running = 0;
    CHECK(PurgeIntoInnerWalls::purge_demand<float>(std::vector<unsigned int>{0, 1, 1, 2, 3}, running, volume_at) == 20.f);
    CHECK(running == 3);
    running = 2;
    CHECK(PurgeIntoInnerWalls::purge_demand<double>(std::vector<unsigned int>{2, 3}, running, volume_at) == 0.);
    CHECK(running == 3);
}

TEST_CASE("Grown purge walls taper one ring per layer", "[PurgeIntoInnerWalls]")
{
    // The single rule behind both the model wall growth and the support wall
    // growth: one ring fewer for every layer in between, never negative.
    CHECK(PurgeIntoInnerWalls::tapered_rings(5, 1) == 4);
    CHECK(PurgeIntoInnerWalls::tapered_rings(5, 4) == 1);
    CHECK(PurgeIntoInnerWalls::tapered_rings(5, 5) == 0);
    CHECK(PurgeIntoInnerWalls::tapered_rings(5, 9) == 0);
    CHECK(PurgeIntoInnerWalls::tapered_rings(0, 1) == 0);
    CHECK(PurgeIntoInnerWalls::tapered_rings(size_t(3), size_t(2)) == 1);
}

TEST_CASE("Inner-wall purge declines geometry caches before changing generated layers", "[PurgeIntoInnerWalls][Regression]")
{
    Model model;
    Print print;
    const DynamicPrintConfig config = purge_config("classic", 1);
    init_purge_print(print, model, config);
    print.process();
    ScopedTemporaryDir cache("purge-wall-cache");
    REQUIRE(print.export_cached_data(cache.string()) == 0);
    const PrintObject *object = print.objects().front();
    const size_t layers = object->layer_count();
    REQUIRE(layers > 0);
    const Layer *first_layer = object->get_layer(0);
    // Loading geometry alone loses purge tags and native support snapshots.
    // A failed import must leave the live slicing state untouched so the
    // caller can use its existing fallback to normal slicing.
    CHECK(print.load_cached_data(cache.string()) == CLI_IMPORT_CACHE_DATA_CAN_NOT_USE);
    CHECK(object->layer_count() == layers);
    CHECK(object->get_layer(0) == first_layer);
    CHECK(object->is_step_done(posPerimeters));
}

TEST_CASE("Inner-wall purge warns only for a measured shortfall without a print-time dialog", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool shortfall = GENERATE(false, true);
    CAPTURE(generator, shortfall);

    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 3);
    // The two 20 x 20 x 1 mm cubes cannot absorb 1000 mm³ on one layer,
    // even when filled with walls. A 1 mm³ transition fits their inner walls.
    config.set_deserialize_strict({
        {"prime_volume", shortfall ? 1000. : 1.}, {"enable_support", false},
        {"use_relative_e_distances", true}, {"layer_change_gcode", "G92 E0\n"},
    });
    init_purge_print(print, model, config);
    std::vector<StringObjectException> warnings;
    const StringObjectException validation = print.validate(&warnings);
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    for (const StringObjectException &warning : warnings)
        CHECK(warning.opt_key != "purge_into_inner_walls");
    print.process();

    const auto state = print.step_state_with_warnings(psWipeTower);
    if (!shortfall) {
        CHECK(state.warnings.empty());
        return;
    }
    REQUIRE(state.warnings.size() == 1);
    const PrintStateBase::Warning &warning = state.warnings.front();
    CHECK(warning.current);
    CHECK_FALSE(warning.message.empty());
    CHECK(warning.level == PrintStateBase::WarningLevel::NOTIFICATION_ONLY);
}

TEST_CASE("Purge allocation ranks preferred objects then loop depth across objects", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool first_preferred = GENERATE(false, true);
    const bool second_preferred = GENERATE(false, true);
    CAPTURE(generator, first_preferred, second_preferred);

    // One object has a large and a small island; the other has a medium island.
    // Global loop ranking must interleave objects, not just sort whole objects
    // by their largest loop. Give the medium island two copies to check ties.
    TriangleMesh first = make_cube(30., 30., 1.);
    TriangleMesh small = make_cube(10., 10., 1.);
    small.translate(45.f, 0.f, 0.f);
    first.merge(small);
    first.translate(40.f, 40.f, 0.f);
    TriangleMesh second = make_cube(20., 20., 1.);
    second.translate(120.f, 40.f, 0.f);
    // Both objects print in filament 1 while the incoming changes are 2 and 3,
    // so every loop is foreign to every change and the depth key decides the
    // order inside a tier, with volume only breaking equal-depth ties. Color
    // matching has its own test below.
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", first_preferred}},
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", second_preferred}},
    };
    DynamicPrintConfig config = purge_config(generator, 3, false, 4);
    config.set_deserialize_strict({{"prime_volume", 1.}, {"enable_support", false}});
    Model model;
    Print print;
    Test::init_print(std::vector<TriangleMesh>{first, second}, print, model, config, &overrides, false);
    ModelInstance *copy = model.objects[1]->add_instance(*model.objects[1]->instances.front());
    copy->set_offset(copy->get_offset() + Vec3d(0., 60., 0.));
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *a = print.objects()[0];
    const PrintObject *b = print.objects()[1];
    REQUIRE(a->instances().size() == 1);
    REQUIRE(b->instances().size() == 2);
    REQUIRE(a->layers().size() > 1);

    LayerTools tools(a->layers()[1]->print_z);
    // Both incoming colors are foreign to both models, so native shells must
    // remain protected while the four available inner-wall copies are claimed.
    // Every claim is therefore inside a foreign tier, which hands out its
    // deepest loop first.
    tools.extruders = {2, 3, 2, 3};
    WipingExtrusions &wiping = tools.wiping_extrusions();
    std::vector<std::vector<const ExtrusionEntityCollection *>> walls(2);
    for (size_t i = 0; i < print.objects().size(); ++i) {
        const PrintObject *object = print.objects()[i];
        const Layer *layer = object->get_layer_at_printz(tools.print_z, EPSILON);
        REQUIRE(layer != nullptr);
        for (const auto &candidate : layer->purge_inner_walls()) {
            const auto *wall = candidate.wall.collection;
            if (wiping.is_overriddable_and_mark(*wall, print.config(), *object, candidate.region->region()))
                walls[i].push_back(wall);
        }
    }
    REQUIRE(walls[0].size() == 2);
    REQUIRE(walls[1].size() == 1);
    if (walls[0][0]->total_volume() < walls[0][1]->total_volume())
        std::swap(walls[0][0], walls[0][1]);
    REQUIRE(walls[0][0]->total_volume() > walls[1][0]->total_volume());
    REQUIRE(walls[1][0]->total_volume() > walls[0][1]->total_volume());
    REQUIRE(walls[0][1]->total_volume() > 1.); // each transition claims exactly one loop

    struct Claim {
        const ExtrusionEntityCollection *wall;
        const PrintObject *object;
        size_t copy;
    };
    // Every wall is larger than the 1 mm3 a single transition asks for, so each
    // transition claims exactly one loop. The claim order is the documented one
    // (see PurgeIntoInnerWalls::wall_before()): the per-object preference tier
    // first, then depth across all objects, so the deepest loop of the layer
    // goes before a smaller loop that merely sits near a visible wall, then the
    // smaller loop, then object and copy order. Restated here rather than called
    // so the test pins the rule instead of the implementation.
    std::vector<Claim> expected;
    for (const PrintObject *object : print.objects()) {
        const Layer *layer = object->get_layer_at_printz(tools.print_z, EPSILON);
        REQUIRE(layer != nullptr);
        for (const auto &candidate : layer->purge_inner_walls()) {
            const auto *wall = candidate.wall.collection;
            if (!wiping.is_overriddable(*wall, print.config(), *object, candidate.region->region()))
                continue;
            for (size_t copy = 0; copy < object->instances().size(); ++copy)
                expected.push_back({wall, object, copy});
        }
    }
    REQUIRE(expected.size() == 4);
    const auto preferred = [](const Claim &claim) {
        return claim.object->config().purge_into_inner_walls_preferred.getBool();
    };
    std::stable_sort(expected.begin(), expected.end(), [&preferred](const Claim &x, const Claim &y) {
        if (preferred(x) != preferred(y))
            return preferred(x);
        if (x.wall->get_max_inset_idx() != y.wall->get_max_inset_idx())
            return x.wall->get_max_inset_idx() > y.wall->get_max_inset_idx(); // deepest first
        if (x.wall->is_existing_purge_wall() != y.wall->is_existing_purge_wall())
            return x.wall->is_existing_purge_wall();                          // reuse before generated
        return x.wall->total_volume() < y.wall->total_volume();               // then the smaller loop
    });

    FilamentChangeStats stats;
    unsigned int previous = 0;
    for (size_t change = 0; change < tools.extruders.size(); ++change) {
        const unsigned int filament = tools.extruders[change];
        CHECK_THAT(wiping.mark_wiping_extrusions(print, previous, filament, 1.f, stats),
                   Catch::Matchers::WithinAbs(0., 1e-5));
        previous = filament;
        for (size_t i = 0; i < expected.size(); ++i) {
            const Claim &claim = expected[i];
            const int actual = wiping.get_extruder_override(claim.wall, claim.object, claim.copy);
            if (i <= change)
                CHECK(actual == int(tools.extruders[i]));
            else
                CHECK(actual < 0);
        }
    }
}

TEST_CASE("Purge claims the loop nearest a matching exterior before a smaller loop", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    CAPTURE(generator);

    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 6, false, 3);
    config.set_deserialize_strict({{"prime_volume", 1.}});
    // One copy keeps the claim sequence readable; per-copy shell reservation is
    // covered by "Purge reserves a native inner shell independently for each copy".
    init_purge_print(print, model, config, true, 1);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *preferred = print.objects().front();
    REQUIRE(preferred->layers().size() > 1);
    const Layer *layer = preferred->layers()[1];

    // A foreign change first, then a change back to the color the object already
    // prints on its exterior, which is what makes a wall a color match.
    LayerTools tools(layer->print_z);
    tools.extruders = {2, 0};
    WipingExtrusions &wiping = tools.wiping_extrusions();
    double smallest_volume = std::numeric_limits<double>::max();
    int nearest_inset = std::numeric_limits<int>::max();
    const ExtrusionEntityCollection *nearest_wall = nullptr;
    for (const LayerRegion *region : layer->regions()) {
        REQUIRE(tools.wall_extruder_id(region->region()) == 0);
        for (const ExtrusionEntity *entity : region->perimeters.entities) {
            const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(entity);
            REQUIRE(wall != nullptr);
            if (!wiping.is_overriddable_and_mark(*wall, print.config(), *preferred, region->region()) ||
                !wall->is_purge_inner_wall_candidate())
                continue;
            smallest_volume = std::min(smallest_volume, wall->total_volume());
            if (wall->get_max_inset_idx() < nearest_inset) {
                nearest_inset = wall->get_max_inset_idx();
                nearest_wall = wall;
            }
        }
    }
    REQUIRE(smallest_volume > 0.);
    REQUIRE(nearest_inset > 0);
    REQUIRE(nearest_wall != nullptr);
    // Concentric loops shrink inward, so the loop nearest the exterior is the
    // largest one while a foreign tier claims its innermost, smallest loop
    // first.
    REQUIRE(nearest_wall->total_volume() > smallest_volume);

    // Small enough for every loop to absorb a change on its own, so one claim
    // ends a change.
    const float demand = float(0.75 * smallest_volume);
    FilamentChangeStats stats;

    // A foreign change must not repaint the loop beside the visible boundary:
    // the claim starts at the innermost loop, which here is also the smallest
    // one and big enough to absorb the whole change.
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 0, 2, demand, stats), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK(wiping.get_extruder_override(nearest_wall, preferred, 0) < 0);
    size_t claims = 0;
    for (const LayerRegion *region : layer->regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities) {
            const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(entity);
            REQUIRE(wall != nullptr);
            const int filament = wiping.get_extruder_override(wall, preferred, 0);
            if (filament < 0)
                continue;
            ++claims;
            CHECK(filament == 2);                            // foreign purge
            CHECK(wall->total_volume() >= double(demand));    // absorbs the whole change
        }
    CHECK(claims == 1);

    // A change into the exterior color is served by color adjacency first: the
    // closest loop wins even though a smaller, deeper loop comes earlier in a
    // foreign tier.
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 2, 0, demand, stats), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK(wiping.get_extruder_override(nearest_wall, preferred, 0) == 0);

    // The per-object preference still wins over a nonpreferred color match.
    const PrintObject *other = print.objects()[1];
    const Layer *other_layer = other->get_layer_at_printz(tools.print_z, EPSILON);
    REQUIRE(other_layer != nullptr);
    for (const LayerRegion *region : other_layer->regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            CHECK(wiping.get_extruder_override(entity, other, 0) < 0);
}

TEST_CASE("Purge reuses inner walls that already print the purge color", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    CAPTURE(generator);

    // Outer walls print filament 1, the user's inner walls filament 2, so a
    // change into filament 2 matches the walls' own color while the visible
    // exterior stays a different color. Those walls need no color change at
    // all, so they are purge capacity and their volume is subtracted from the
    // flush that still has to be placed.
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 4, false, 3);
    // Region-level: every object keeps its own exterior filament but prints its
    // inner walls in filament 2 (normalize_fdm only fills a 0, so the object
    // "extruder" overrides below do not reset it).
    config.set_deserialize_strict({{"prime_volume", 1.}, {"inner_wall_filament_id", 2}});
    TriangleMesh first = make_cube(20., 20., 1.);
    first.translate(40.f, 40.f, 0.f);
    TriangleMesh second = make_cube(20., 20., 1.);
    second.translate(80.f, 40.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", true}},
        {{"extruder", 2}},
    };
    Test::init_print({first, second}, print, model, config, &overrides, false);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *preferred = print.objects().front();
    REQUIRE(preferred->layers().size() > 1);
    const Layer *layer = preferred->layers()[1];
    REQUIRE(layer->regions().size() == 1);
    const PrintRegion &region = layer->regions().front()->region();

    LayerTools tools(layer->print_z);
    tools.extruders = {1}; // the inner walls' own filament
    WipingExtrusions &wiping = tools.wiping_extrusions();

    const ExtrusionEntityCollection *nearest = nullptr;
    int nearest_inset = std::numeric_limits<int>::max();
    for (const auto &candidate : layer->purge_inner_walls()) {
        if (!wiping.is_overriddable_and_mark(*candidate.wall.collection, print.config(), *preferred, candidate.region->region()))
            continue;
        if (candidate.wall.collection->get_max_inset_idx() < nearest_inset) {
            nearest_inset = candidate.wall.collection->get_max_inset_idx();
            nearest = candidate.wall.collection;
        }
    }
    REQUIRE(nearest != nullptr);
    REQUIRE(nearest_inset > 0);
    // Premise: the exterior prints filament 1, the loop nearest the exterior
    // prints filament 2 - the purge color of the change below.
    REQUIRE(tools.wall_extruder_id(region) == 0);
    REQUIRE(tools.extruder(*nearest, region) == 1);

    FilamentChangeStats stats;
    const float demand = float(0.75 * nearest->total_volume());
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 0, 1, demand, stats), Catch::Matchers::WithinAbs(0., 1e-5));

    // The nearest loop is claimed even though it sits right behind a different
    // exterior color: its own color already is the purge color, and its volume
    // is subtracted from the flush, so a single loop absorbs the whole change.
    CHECK(wiping.get_extruder_override(nearest, preferred, 0) == 1);
    size_t claims = 0;
    for (const ExtrusionEntity *entity : layer->regions().front()->perimeters.entities) {
        const auto *wall = dynamic_cast<const ExtrusionEntityCollection *>(entity);
        REQUIRE(wall != nullptr);
        if (!wall->is_purge_inner_wall_candidate())
            continue; // the visible exterior stays untouched
        if (wiping.get_extruder_override(wall, preferred, 0) >= 0)
            ++claims;
    }
    CHECK(claims == 1);
    // The nonpreferred object's walls are not needed at all.
    const PrintObject *other = print.objects()[1];
    const Layer *other_layer = other->get_layer_at_printz(tools.print_z, EPSILON);
    REQUIRE(other_layer != nullptr);
    for (const LayerRegion *other_region : other_layer->regions())
        for (const ExtrusionEntity *entity : other_region->perimeters.entities)
            CHECK(wiping.get_extruder_override(entity, other, 0) < 0);
}

TEST_CASE("Purge keeps the loop behind a claimed wall in its own color", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    CAPTURE(generator);

    // Outer walls on filament 1, the user's inner walls on filament 2. A foreign
    // change may take a deep loop, but the loop that hides it has to keep
    // printing the inner-wall filament instead of being repainted in the
    // region's exterior filament.
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 4, false, 4);
    config.set_deserialize_strict({{"prime_volume", 1.}, {"inner_wall_filament_id", 2}});
    TriangleMesh first = make_cube(20., 20., 1.);
    first.translate(40.f, 40.f, 0.f);
    TriangleMesh second = make_cube(20., 20., 1.);
    second.translate(80.f, 40.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", true}},
        {{"extruder", 2}},
    };
    Test::init_print({first, second}, print, model, config, &overrides, false);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *preferred = print.objects().front();
    REQUIRE(preferred->layers().size() > 1);
    const Layer *layer = preferred->layers()[1];
    REQUIRE(layer->regions().size() == 1);
    const PrintRegion &region = layer->regions().front()->region();

    LayerTools tools(layer->print_z);
    tools.extruders = {3}; // foreign to both the exterior and the inner walls
    WipingExtrusions &wiping = tools.wiping_extrusions();
    const ExtrusionEntityCollection *nearest = nullptr;
    int nearest_inset = std::numeric_limits<int>::max();
    int deepest_inset = -1;
    for (const auto &candidate : layer->purge_inner_walls()) {
        if (!wiping.is_overriddable_and_mark(*candidate.wall.collection, print.config(), *preferred,
                                             candidate.region->region()))
            continue;
        const int inset = candidate.wall.collection->get_max_inset_idx();
        if (inset < nearest_inset) {
            nearest_inset = inset;
            nearest = candidate.wall.collection;
        }
        deepest_inset = std::max(deepest_inset, inset);
    }
    REQUIRE(nearest != nullptr);
    REQUIRE(deepest_inset > nearest_inset); // a wall sits behind the boundary loop
    // Premise: the boundary loop prints the inner-wall filament, not the exterior.
    REQUIRE(tools.wall_extruder_id(region) == 0);
    REQUIRE(tools.extruder(*nearest, region) == 1);

    // Small enough that the smallest (deepest) loop absorbs the whole change.
    FilamentChangeStats stats;
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 0, 3, 0.5f, stats), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK(wiping.get_extruder_override(nearest, preferred, 0) < 0);
    // The unclaimed loop keeps inner-wall filament 2 (index 1).
    CHECK(tools.extruder(*nearest, region) == 1);
}

TEST_CASE("Purge fills a foreign wall stack from the inside out", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    CAPTURE(generator);

    // One object and one island keep the claim sequence readable: concentric
    // loops and their insets line up one to one, so depth is unambiguous.
    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 4, false, 4);
    config.set_deserialize_strict({{"prime_volume", 1.}});
    TriangleMesh cube = make_cube(20., 20., 1.);
    cube.translate(40.f, 40.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}, {"purge_into_inner_walls_preferred", true}},
    };
    Test::init_print(std::vector<TriangleMesh>{cube}, print, model, config, &overrides, false);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layers().size() > 1);
    const Layer *layer = object->layers()[1];
    REQUIRE(layer->regions().size() == 1);
    const PrintRegion &region = layer->regions().front()->region();

    LayerTools tools(layer->print_z);
    tools.extruders = {3}; // foreign to the walls and to the exterior
    WipingExtrusions &wiping = tools.wiping_extrusions();

    std::vector<const ExtrusionEntityCollection *> walls;
    for (const auto &candidate : layer->purge_inner_walls())
        if (wiping.is_overridable(*candidate.wall.collection, print.config(), *object, region))
            walls.push_back(candidate.wall.collection);
    REQUIRE(walls.size() >= 3);
    // Premise: the loop stack prints a filament the incoming flush is foreign
    // to, so the change runs through the foreign tier.
    for (const ExtrusionEntityCollection *wall : walls)
        REQUIRE(tools.extruder(*wall, region) != 3);

    // The claim order is by depth, so the innermost loop comes first. It is also
    // the smallest one, which keeps the premise simple: a demand slightly above
    // its volume cannot be served by that one loop alone.
    std::map<int, double> volume_at_inset;
    for (const ExtrusionEntityCollection *wall : walls)
        volume_at_inset[wall->get_max_inset_idx()] += wall->total_volume();
    REQUIRE(volume_at_inset.size() >= 3);
    const auto deepest = volume_at_inset.rbegin(); // largest inset = innermost loop
    REQUIRE(deepest->first > volume_at_inset.begin()->first);
    REQUIRE(deepest->second < volume_at_inset.begin()->second);

    // More than the innermost loop holds, so one loop cannot serve the flush and
    // the next loop in the stack has to.
    FilamentChangeStats stats;
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 0, 3, float(1.2 * deepest->second), stats),
               Catch::Matchers::WithinAbs(0., 1e-5));

    // The claims are the deepest loops, in order and with no gap: the purge
    // color is one contiguous band and the loops beside the visible boundary
    // still print the walls' filament. Fitting each remainder on its own would
    // leave a native loop inside the band instead.
    std::vector<int> claimed;
    std::vector<int> available;
    for (const ExtrusionEntityCollection *wall : walls) {
        available.push_back(wall->get_max_inset_idx());
        if (wiping.get_extruder_override(wall, object, 0) >= 0)
            claimed.push_back(wall->get_max_inset_idx());
    }
    REQUIRE(claimed.size() >= 2);
    std::sort(claimed.rbegin(), claimed.rend());
    std::sort(available.rbegin(), available.rend());
    CHECK(std::equal(claimed.begin(), claimed.end(), available.begin()));
    CHECK(claimed.size() < available.size());
}

TEST_CASE("Purge spots a transition its walls already print the color for", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    CAPTURE(generator);

    Model model;
    Print print;
    DynamicPrintConfig config = purge_config(generator, 4, false, 4);
    config.set_deserialize_strict({{"prime_volume", 1.}});
    init_purge_print(print, model, config, true, 1);
    print.process();
    REQUIRE(print.objects().size() == 2);
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layers().size() > 1);
    const Layer *layer = object->layers()[1];
    REQUIRE(layer->regions().size() == 1);
    const PrintRegion &region = layer->regions().front()->region();

    LayerTools tools(layer->print_z);
    tools.extruders = {0, 1, 2, 3};
    WipingExtrusions &wiping = tools.wiping_extrusions();

    // The layer's allocation order is decided before any transition is served,
    // so a transition that would have to repaint a foreign wall has to be told
    // apart from one that can use a wall already printing its color. This
    // predicate makes that split, and it must agree with the allocator's own
    // matching rule.
    const auto &candidates = layer->purge_inner_walls();
    REQUIRE(!candidates.empty());
    std::set<unsigned int> printed;
    for (const auto &candidate : candidates) {
        const unsigned int own = unsigned(tools.extruder(*candidate.wall.collection, region));
        CAPTURE(own);
        CHECK(wiping.has_matching_target(print, own));
        printed.insert(own);
    }
    // A region's exterior color counts as well, on every object of the layer.
    for (const PrintObject *layer_object : print.objects()) {
        const Layer *object_layer = layer_object->get_layer_at_printz(tools.print_z, EPSILON);
        REQUIRE(object_layer != nullptr);
        for (const LayerRegion *layerm : object_layer->regions()) {
            const unsigned int exterior = unsigned(tools.wall_extruder_id(layerm->region()));
            CAPTURE(exterior);
            CHECK(wiping.has_matching_target(print, exterior));
            printed.insert(exterior);
        }
    }
    // A filament nothing on the layer prints can only be served by repainting a
    // foreign wall, so that transition belongs to the second pass.
    unsigned int foreign = 0;
    while (printed.count(foreign) != 0)
        ++foreign;
    CAPTURE(foreign);
    CHECK_FALSE(wiping.has_matching_target(print, foreign));
}

TEST_CASE("Only purged inner wall seams scatter across layers and copies", "[PurgeIntoInnerWalls][Regression]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool enabled = GENERATE(false, true);
    const bool infill_first = GENERATE(false, true);
    CAPTURE(generator, enabled, infill_first);
    DynamicPrintConfig config = purge_config(generator, 6);
    config.set_deserialize_strict({
        {"purge_into_inner_walls", enabled},
        {"is_infill_first", infill_first},
        {"prime_volume", 12.}, // more than one 20 mm cube loop, so both copies purge
        {"line_width", 0.45},
        {"inner_wall_line_width", 0.45},
        {"outer_wall_line_width", 0.45},
        {"initial_layer_line_width", 0.45},
        {"seam_position", "back"},
        {"staggered_inner_seams", false},
        {"seam_slope_type", "none"},
        {"enable_arc_fitting", false},
        {"wipe_inward", false},
        {"use_relative_e_distances", false},
        {"change_filament_gcode", ";TEST_PURGE_START\n"},
    });
    Model model;
    Print print;
    init_purge_print(print, model, config, true, 2);

    struct SeamSamples {
        std::map<std::pair<size_t, size_t>, Points> purged;
        std::set<float> purge_layers;
        size_t ordinary_inner = 0;
        size_t exterior = 0;
    };
    const auto collect = [&](const std::string &gcode) {
        SeamSamples samples;
        bool purging = false;
        bool travelled = true;
        std::string role;
        GCodeReader reader;
        reader.apply_config(config);
        reader.parse_buffer(gcode, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
            const std::string comment(line.comment());
            if (comment == "TEST_PURGE_START")
                purging = true;
            else if (comment.find("PURGING FINISHED") != std::string::npos)
                purging = false;
            else if (comment.rfind("TYPE:", 0) == 0)
                role = comment.substr(5);
            const bool move = line.cmd_is("G0") || line.cmd_is("G1") || line.cmd_is("G2") || line.cmd_is("G3");
            if (move && !line.extruding(state) && line.dist_XY(state) > 1e-4f)
                travelled = true;
            if (!line.extruding(state) || line.dist_XY(state) <= 1e-4f || !travelled)
                return;
            travelled = false;
            if (state.z() <= 0.201f || (role != "Inner wall" && role != "Outer wall"))
                return;
            for (size_t object = 0; object < print.objects().size(); ++object) {
                const auto &instances = print.objects()[object]->instances();
                for (size_t copy = 0; copy < instances.size(); ++copy) {
                    const BoundingBoxf3 box = instances[copy].get_bounding_box();
                    if (state.x() < box.min.x() || state.x() > box.max.x() ||
                        state.y() < box.min.y() || state.y() > box.max.y())
                        continue;
                    const Point local = Point::new_scale(state.x() - box.min.x(), state.y() - box.min.y());
                    if (enabled && purging && role == "Inner wall") {
                        samples.purged[{object, copy}].push_back(local);
                        samples.purge_layers.insert(state.z());
                    } else {
                        // Independent purge starts must not move the exterior's
                        // cached rear seam, or the ordinary inner-wall seams.
                        CHECK(state.y() > box.center().y());
                        if (role == "Inner wall")
                            ++samples.ordinary_inner;
                        else
                            ++samples.exterior;
                    }
                    return;
                }
            }
            FAIL("Wall seam does not belong to any printed instance");
        });
        return samples;
    };
    const SeamSamples samples = collect(Test::gcode(print));
    REQUIRE(samples.exterior > 0);
    REQUIRE(samples.ordinary_inner > 0);
    if (enabled) {
        REQUIRE(samples.purged.size() == 2);
        CHECK(samples.purge_layers.size() > 1);
        for (const auto &[instance, seams] : samples.purged) {
            CAPTURE(instance.first, instance.second);
            REQUIRE(seams.size() > 2);
            CHECK(std::any_of(seams.begin(), seams.end(), [](const Point &p) { return p.y() < scale_(10.); }));
            Points distinct = seams;
            std::sort(distinct.begin(), distinct.end());
            distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
            CHECK(distinct.size() > 2);
        }
        CHECK(samples.purged.at({0, 0}).front() != samples.purged.at({0, 1}).front());
        // Re-exporting must replay the sequence, not continue a previous export.
        const SeamSamples replay = collect(Test::gcode(print));
        CHECK(replay.purged == samples.purged);
    } else
        CHECK(samples.purged.empty());
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
