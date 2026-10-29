#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
#include "libslic3r/Support/PurgeSupport.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
DynamicPrintConfig support_purge_config(const char *style, double demand, unsigned int filaments = 2)
{
    const bool normal = std::string(style) == "normal";
    auto config = Test::multifilament_config(filaments, {
        {"purge_into_inner_walls", true}, {"purge_into_inner_walls_preferred", false},
        {"enable_prime_tower", false}, {"purge_in_prime_tower", false}, {"prime_volume", demand},
        {"single_extruder_multi_material", true}, {"print_sequence", "by layer"},
        {"wall_generator", "classic"}, {"wall_loops", 1},
        {"only_one_wall_first_layer", false}, {"only_one_wall_top", false},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"top_shell_layers", 1}, {"bottom_shell_layers", 1},
        {"enable_support", true}, {"support_type", normal ? "normal(auto)" : "tree(auto)"},
        {"support_style", normal ? "default" : style}, {"support_threshold_angle", 30},
        {"support_on_build_plate_only", true}, {"independent_support_layer_height", false},
        {"support_base_pattern", "rectilinear"}, {"support_base_pattern_spacing", 8.},
        {"support_interface_top_layers", 1}, {"support_interface_bottom_layers", 0},
        {"tree_support_wall_count", 2}, {"tree_support_branch_diameter", 8.},
        {"tree_support_branch_diameter_organic", 8.},
        {"support_filament", 1}, {"support_interface_filament", 1},
        {"flush_into_objects", false}, {"flush_into_infill", false}, {"flush_into_support", false},
        {"brim_type", "no_brim"}, {"skirt_loops", 0},
    });
    config.option<ConfigOptionFloats>("filament_flow_ratio")->values.assign(filaments, 1.);
    return config;
}

void init_support_purge(Print &print, Model &model, const DynamicPrintConfig &config, bool extra_copy = false)
{
    // A narrow model stem quickly becomes solid, while its wide cap needs
    // sizeable supports with room for more than the UI's two support walls.
    TriangleMesh shape = make_cube(2., 2., 4.2);
    shape.translate(7.f, 7.f, 0.f);
    TriangleMesh cap = make_cube(16., 16., 0.8);
    cap.translate(0.f, 0.f, 4.f); // overlap the stem, not a coplanar join
    shape.merge(cap);
    TriangleMesh first = shape, second = shape;
    first.translate(40.f, 40.f, 0.f);
    second.translate(80.f, 40.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}}, {{"extruder", 2}},
    };
    Test::init_print(std::vector<TriangleMesh>{first, second}, print, model, config, &overrides, false);
    if (extra_copy) {
        ModelObject &object = *model.objects.front();
        ModelInstance *copy = object.add_instance(*object.instances.front());
        copy->set_offset(copy->get_offset() + Vec3d(0., 40., 0.));
        print.apply(model, config);
    }
}

size_t support_overrides(const Print &print)
{
    size_t count = 0;
    for (const LayerTools &tools : print.tool_ordering())
        for (const PrintObject *object : print.objects())
            if (const SupportLayer *support = object->get_support_layer_at_printz(tools.print_z, EPSILON))
                for_each_extrusion_leaf(support->support_fills, [&](const ExtrusionEntity &entity) {
                    for (size_t copy = 0; copy < object->instances().size(); ++copy)
                        if (tools.wiping_extrusions().is_entity_overridden(&entity, object, copy))
                            ++count;
                });
    return count;
}

void check_support_export(Print &print)
{
    // Explicit native support filaments in these fixtures. Summing by
    // filament catches both lost unclaimed paths and duplicate support output,
    // including when two instances have different leaf overrides.
    std::vector<double> expected(print.config().filament_diameter.values.size(), 0.);
    for (const LayerTools &tools : print.tool_ordering())
        for (const PrintObject *object : print.objects())
            if (const SupportLayer *support = object->get_support_layer_at_printz(tools.print_z, EPSILON))
                for_each_extrusion_leaf(support->support_fills, [&](const ExtrusionEntity &entity) {
                    if (!is_support(entity.role()))
                        return;
                    const unsigned int native = (entity.role() == erSupportMaterialInterface ?
                        object->config().support_interface_filament.value : object->config().support_filament.value) - 1;
                    for (size_t copy = 0; copy < object->instances().size(); ++copy) {
                        const int override = tools.wiping_extrusions().get_extruder_override(&entity, object, copy);
                        expected.at(override < 0 ? native : unsigned(override)) += entity.total_volume();
                    }
                });
    GCodeProcessorResult result;
    Test::gcode(print, &result);
    std::vector<double> actual(expected.size(), 0.);
    size_t model_outer_moves = 0;
    size_t support_ironing_moves = 0;
    for (const auto &move : result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        if (is_support(move.extrusion_role)) {
            const double diameter = print.config().filament_diameter.get_at(move.extruder_id);
            actual.at(move.extruder_id) += move.delta_extruder * PI * diameter * diameter * 0.25;
        } else if (move.extrusion_role == erIroning) {
            ++support_ironing_moves;
            CHECK(int(move.extruder_id) == print.objects().front()->config().support_interface_filament.value - 1);
        } else if (move.extrusion_role == erExternalPerimeter) {
            ++model_outer_moves;
            size_t matches = 0;
            for (const PrintObject *object : print.objects())
                for (const PrintInstance &instance : object->instances()) {
                    const BoundingBoxf3 box = instance.get_bounding_box();
                    if (move.position.x() >= box.min.x() - 0.05 && move.position.x() <= box.max.x() + 0.05 &&
                        move.position.y() >= box.min.y() - 0.05 && move.position.y() <= box.max.y() + 0.05) {
                        ++matches;
                        CHECK(int(move.extruder_id) == object->model_object()->config.opt_int("extruder") - 1);
                    }
                }
            CHECK(matches == 1);
        }
    }
    REQUIRE(model_outer_moves > 0);
    if (print.objects().front()->config().support_ironing.getBool())
        REQUIRE(support_ironing_moves > 0);
    for (size_t filament = 0; filament < expected.size(); ++filament) {
        CAPTURE(filament);
        // Loop seams/rounding remove a small amount of E during export.
        if (expected[filament] > 0.)
            CHECK_THAT(actual[filament], WithinRel(expected[filament], 0.05));
        else
            CHECK_THAT(actual[filament], WithinAbs(0., 0.01));
    }
}
} // namespace

TEST_CASE("Supports absorb residual purge after model walls and grow past two loops", "[PurgeSupport][PurgeIntoInnerWalls][Regression]")
{
    const char *style = GENERATE("normal", "tree_hybrid", "organic");
    CAPTURE(style);
    Model model;
    Print print;
    const auto config = support_purge_config(style, 60.);
    init_support_purge(print, model, config, true);
    print.process();
    REQUIRE(support_overrides(print) > 0);
    size_t grown_regions = 0;
    size_t purged_support_loops = 0;
    bool copies_differ = false;
    for (const LayerTools &tools : print.tool_ordering()) {
        bool support_claimed = false;
        for (const PrintObject *object : print.objects())
            if (const SupportLayer *support = object->get_support_layer_at_printz(tools.print_z, EPSILON)) {
                CHECK(object->config().tree_support_wall_count.value == 2); // runtime growth, not a preset mutation
                for (const PurgeSupportRegion &region : support->purge_support_regions)
                    if (region.wall_count() > 2)
                        ++grown_regions;
                for_each_extrusion_leaf(support->support_fills, [&](const ExtrusionEntity &entity) {
                    const auto &wiping = tools.wiping_extrusions();
                    for (size_t copy = 0; copy < object->instances().size(); ++copy)
                        if (wiping.is_entity_overridden(&entity, object, copy)) {
                            support_claimed = true;
                            if (dynamic_cast<const ExtrusionLoop *>(&entity))
                                ++purged_support_loops; // no inner-only model tag is required
                        }
                    if (object->instances().size() > 1 &&
                        wiping.get_extruder_override(&entity, object, 0) != wiping.get_extruder_override(&entity, object, 1))
                        copies_differ = true;
                });
            }
        if (support_claimed)
            for (const PrintObject *object : print.objects())
                if (const Layer *layer = object->get_layer_at_printz(tools.print_z, EPSILON))
                    for (const LayerRegion *region : layer->regions())
                        for (const ExtrusionEntity *entity : region->perimeters.entities)
                            if (const auto *walls = dynamic_cast<const ExtrusionEntityCollection *>(entity))
                                if (tools.wiping_extrusions().is_overriddable(*walls, print.config(), *object, region->region()))
                                    for (size_t copy = 0; copy < object->instances().size(); ++copy)
                                        CHECK(tools.wiping_extrusions().is_entity_overridden(walls, object, copy));
    }
    CHECK(grown_regions > 0);
    CHECK(purged_support_loops > 0);
    CHECK(copies_differ);
    check_support_export(print);
}

TEST_CASE("Sufficient model walls leave support paths and wall counts unclaimed", "[PurgeSupport][PurgeIntoInnerWalls]")
{
    Model model;
    Print print;
    const auto config = support_purge_config("normal", 0.01);
    init_support_purge(print, model, config);
    print.process();
    CHECK(support_overrides(print) == 0);
    size_t support_regions = 0;
    for (const PrintObject *object : print.objects())
        for (const SupportLayer *support : object->support_layers())
            for (const PurgeSupportRegion &region : support->purge_support_regions) {
                ++support_regions;
                CHECK(region.wall_count() <= 2);
            }
    REQUIRE(support_regions > 0);
}

TEST_CASE("Soluble and dedicated support interfaces keep their native filament", "[PurgeSupport][PurgeIntoInnerWalls][Regression]")
{
    const bool soluble = GENERATE(false, true);
    Model model;
    Print print;
    auto config = support_purge_config("normal", 60., 3);
    config.set_deserialize_strict({
        {"support_interface_filament", 3},
        {"support_ironing", true},
        {"filament_soluble", soluble ? "0,0,1" : "0,0,0"},
        {"filament_is_support", soluble ? "0,0,0" : "0,0,1"},
    });
    init_support_purge(print, model, config);
    print.process();
    REQUIRE(support_overrides(print) > 0); // protection must not disable body purge too
    size_t protected_interfaces = 0;
    for (const LayerTools &tools : print.tool_ordering())
        for (const PrintObject *object : print.objects())
            if (const SupportLayer *support = object->get_support_layer_at_printz(tools.print_z, EPSILON))
                for_each_extrusion_leaf(support->support_fills, [&](const ExtrusionEntity &entity) {
                    if (entity.role() == erSupportMaterialInterface) {
                        ++protected_interfaces;
                        CHECK_FALSE(tools.wiping_extrusions().is_support_overriddable(entity.role(), *object));
                        CHECK_FALSE(tools.wiping_extrusions().is_entity_overridden(&entity, object, 0));
                    }
                });
    REQUIRE(protected_interfaces > 0);
    check_support_export(print);
}

TEST_CASE("Replanning removes unneeded support growth and disabling purge regenerates native supports", "[PurgeSupport][PurgeIntoInnerWalls][Regression]")
{
    Model model;
    Print print;
    auto config = support_purge_config("normal", 60.);
    init_support_purge(print, model, config);
    print.process();
    size_t grown = 0;
    for (const PrintObject *object : print.objects())
        for (const SupportLayer *support : object->support_layers())
            for (const PurgeSupportRegion &region : support->purge_support_regions)
                if (region.wall_count() > 2)
                    ++grown;
    REQUIRE(grown > 0);
    config.set_deserialize_strict({{"prime_volume", 0.01}});
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psWipeTower));
    for (const PrintObject *object : print.objects())
        CHECK_FALSE(object->is_step_done(posSimplifySupportPath));
    print.process();
    CHECK(support_overrides(print) == 0);
    for (const PrintObject *object : print.objects())
        for (const SupportLayer *support : object->support_layers())
            for (const PurgeSupportRegion &region : support->purge_support_regions)
                CHECK(region.wall_count() <= 2);

    config.set_deserialize_strict({{"purge_into_inner_walls", false}});
    print.apply(model, config);
    for (const PrintObject *object : print.objects())
        CHECK_FALSE(object->is_step_done(posSupportMaterial));
    print.process();
    CHECK(support_overrides(print) == 0);
    for (const PrintObject *object : print.objects())
        for (const SupportLayer *support : object->support_layers())
            CHECK(support->purge_support_regions.empty());
}

TEST_CASE("Protected material changes do not add a fictitious purge transition", "[PurgeSupport][PurgeIntoInnerWalls][Regression]")
{
    Model model;
    Print print;
    auto config = support_purge_config("normal", 60., 4);
    config.set_deserialize_strict({{"filament_soluble", "0,0,1,0"}});
    init_support_purge(print, model, config);
    print.process();
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    LayerTools tools(print.objects().front()->support_layers().front()->print_z);
    tools.extruders = {0, 2, 1, 3};
    WipingExtrusions &wiping = tools.wiping_extrusions();
    REQUIRE(wiping.is_support_overriddable_and_mark(erMixed, *print.objects().front()));
    const float capacity = wiping.purge_capacity(print, 2);
    REQUIRE(capacity > 0.f);
    const float demand = capacity * 0.75f;
    config.set_deserialize_strict({{"prime_volume", double(demand)}});
    print.apply(model, config);

    // Enter on soluble filament 2: only 1 -> 3 can purge into this layer.
    // Starting the scale calculation at the first *eligible* change's old
    // filament would invent 1 -> 0 and wrongly halve the usable capacity.
    FilamentChangeStats stats;
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 2, 0, demand, stats), WithinAbs(demand, 1e-4));
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 0, 2, demand, stats), WithinAbs(demand, 1e-4));
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 2, 1, demand, stats), WithinAbs(demand, 1e-4));
    CHECK_THAT(wiping.mark_wiping_extrusions(print, 1, 3, demand, stats), WithinAbs(0., 1e-4));
}
