#include <sstream>
#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Print.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
double fill_area(const LayerRegion &region)
{
    double area = 0.;
    for (const auto &surface : region.fill_surfaces.surfaces)
        area += surface.area();
    return area;
}
}

TEST_CASE("A local purge wall plan adds tagged inner loops without changing density", "[PurgeInnerWalls]")
{
    const char *generator = GENERATE("classic", "arachne");
    const bool alternate = GENERATE(false, true);
    const char *density = GENERATE("0%", "15%");
    CAPTURE(generator, alternate, density);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"wall_generator", generator},
        {"wall_loops", 2},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"alternate_extra_wall", alternate},
        {"sparse_infill_density", density},
        {"only_one_wall_top", false},
        {"only_one_wall_first_layer", false},
        {"top_shell_layers", 3},
        {"bottom_shell_layers", 3},
        {"spiral_mode", false}
    });
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    auto *object = print.get_object(0);
    print.process();
    REQUIRE(object->layers().size() > 12);
    auto *layer = object->get_layer(11); // odd, well below the top and above the bottom
    REQUIRE(layer->regions().size() == 1);
    auto *region = layer->get_region(0);
    const auto baseline = region->perimeters.flatten();
    const double baseline_fill_area = fill_area(*region);
    REQUIRE(baseline_fill_area > 0.);
    const std::string original_density = region->region().config().opt_serialize("sparse_infill_density");
    const std::string original_wall_loops = region->region().config().opt_serialize("wall_loops");
    REQUIRE(region->perimeters.purge_inner_wall_candidates().empty());

    // Exercise the geometry input independently of automatic demand planning.
    // The full pipeline tests below additionally verify support and allocation.
    region->purge_inner_wall_plan.extra_loops = 3;
    layer->make_perimeters();
    const auto candidates = region->perimeters.purge_inner_wall_candidates();
    REQUIRE(candidates.size() == 3);
    const int normal_count = 2 + (alternate && std::string(density) != "0%" ? 1 : 0);
    for (size_t i = 0; i < candidates.size(); ++i) {
        REQUIRE(candidates[i]->inset_idx == normal_count + 2 - int(i));
        REQUIRE(candidates[i]->role() == erPerimeter);
        REQUIRE(candidates[i]->total_volume() > 0.);
    }
    REQUIRE(region->perimeters.flatten().entities.size() == baseline.entities.size() + 3);
    REQUIRE(fill_area(*region) < baseline_fill_area);
    REQUIRE(region->region().config().opt_serialize("sparse_infill_density") == original_density);
    REQUIRE(region->region().config().opt_serialize("wall_loops") == original_wall_loops);

    // A plan reset must not leave tagged geometry in a regenerated layer.
    region->purge_inner_wall_plan = {};
    layer->make_perimeters();
    REQUIRE(region->perimeters.purge_inner_wall_candidates().empty());
    REQUIRE(region->perimeters.flatten().entities.size() == baseline.entities.size());
    REQUIRE_THAT(fill_area(*region), Catch::Matchers::WithinRel(baseline_fill_area, 1e-6));
}

namespace {
DynamicPrintConfig purge_config(const char *generator, double volume, bool tower = false)
{
    auto config = multifilament_config(2, {
        {"wall_generator", generator}, {"wall_loops", 2},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"top_shell_layers", 3}, {"bottom_shell_layers", 3},
        {"top_shell_thickness", 0.6}, {"bottom_shell_thickness", 0.6},
        {"sparse_infill_density", "0%"}, {"alternate_extra_wall", false},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"flush_into_inner_walls", true}, {"flush_inner_walls_max_extra_loops", 0},
        {"flush_into_infill", false}, {"flush_into_objects", false}, {"flush_into_support", false},
        {"enable_prime_tower", tower}, {"purge_in_prime_tower", true},
        {"single_extruder_multi_material", true}, {"enable_mixed_color_sublayer", false},
        {"wipe_tower_x", 70}, {"wipe_tower_y", 70},
        {"flush_multiplier", "1"}, {"filament_minimal_purge_on_wipe_tower", "0,0"},
        {"seam_gap", "0%"}, {"spiral_mode", false}, {"skirt_loops", 0},
        {"print_flow_ratio", 1.}, {"filament_flow_ratio", "1,1"},
        {"set_other_flow_ratios", false}, {"gcode_comments", true},
        {"change_filament_gcode", "; TEST_TOOLCHANGE flush={flush_length}\nT[next_extruder]\n"}
    });
    config.set_deserialize_strict("flush_volumes_matrix", "0," + std::to_string(volume) + "," + std::to_string(volume) + ",0");
    return config;
}

void init_two_color_cube(Print &print, Model &model, const DynamicPrintConfig &config, const TriangleMesh &mesh = cube(20.))
{
    init_print({mesh}, print, model, config);
    DynamicPrintConfig upper;
    upper.set_key_value("extruder", new ConfigOptionInt(2));
    upper.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    model.objects.front()->layer_config_ranges[{8., 20.}].assign_config(std::move(upper));
    print.apply(model, config);
    // See test_wipe_tower: normalize_fdm_2 needs settled regions to retain a tower.
    print.apply(model, config);
}
}

TEST_CASE("A towerless material transition is absorbed by supported hidden walls", "[PurgeInnerWalls]")
{
    const char *generator = GENERATE("classic", "arachne");
    const char *sequence = GENERATE("inner wall/outer wall", "outer wall/inner wall");
    CAPTURE(generator, sequence);
    auto config = purge_config(generator, 15.);
    config.set_deserialize_strict("wall_sequence", sequence);
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    const std::string output = gcode(print);
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE_FALSE(print.has_wipe_tower());
    REQUIRE(print.wipe_tower_data().tool_changes.empty());
    REQUIRE(stats.requested > 0.);
    REQUIRE(stats.inner_walls > 0.);
    REQUIRE_THAT(stats.reduced, Catch::Matchers::WithinAbs(0., 1e-5));
    REQUIRE(stats.affected_layers.empty());
    REQUIRE(output.find("; PURGE INNER WALLS") != std::string::npos);
    std::istringstream lines(output);
    std::string line;
    bool in_purge = false;
    int previous_inset = 0;
    size_t purge_loops = 0;
    const std::string inset_tag = "; PURGE INNER WALL inset=";
    while (std::getline(lines, line)) {
        if (line == "; PURGE INNER WALLS") { in_purge = true; previous_inset = std::numeric_limits<int>::max(); }
        else if (line == "; END PURGE INNER WALLS") in_purge = false;
        else if (in_purge) {
            REQUIRE(line.find("; FEATURE: Outer wall") == std::string::npos);
            REQUIRE(line.find("; FEATURE: Top surface") == std::string::npos);
            if (line.rfind(inset_tag, 0) == 0) {
                const int inset = std::stoi(line.substr(inset_tag.size()));
                REQUIRE(inset >= 2);
                REQUIRE(inset <= previous_inset);
                previous_inset = inset;
                ++purge_loops;
            }
        }
    }
    REQUIRE(purge_loops > 0);
    REQUIRE(output.find("; TEST_TOOLCHANGE flush=0") != std::string::npos);
    REQUIRE(output.find("; purge reduced [mm3] = 0.000") != std::string::npos);
    REQUIRE(print.step_state_with_warnings(psWipeTower).warnings.empty());
    for (const auto *layer : print.get_object(0)->layers())
        for (const auto *region : layer->regions())
            REQUIRE_THAT(region->region().config().sparse_infill_density.value, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Insufficient towerless wall capacity is explicitly reduced and warned", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 10000.);
    config.set_deserialize_strict({{"flush_inner_walls_max_extra_loops", 1}});
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.inner_walls > 0.);
    REQUIRE(stats.reduced > 0.);
    REQUIRE(stats.max_shortfall > 0.);
    REQUIRE_FALSE(stats.affected_layers.empty());
    REQUIRE_FALSE(stats.affected_pairs.empty());
    REQUIRE_THAT(stats.requested, Catch::Matchers::WithinAbs(stats.inner_walls + stats.other + stats.reduced, 1e-3));
    const auto warnings = print.step_state_with_warnings(psWipeTower).warnings;
    REQUIRE(std::any_of(warnings.begin(), warnings.end(), [](const auto &warning) {
        return warning.level == PrintStateBase::WarningLevel::CRITICAL &&
               warning.message_id == PrintStateBase::SlicingReducedInnerWallPurge;
    }));
}

TEST_CASE("A prime tower receives purge exceeding safe wall capacity", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 1000., true);
    config.set_deserialize_strict({{"flush_inner_walls_max_extra_loops", 1}});
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    REQUIRE(print.has_wipe_tower());
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.inner_walls > 0.);
    REQUIRE(stats.tower > 0.);
    REQUIRE_THAT(stats.reduced, Catch::Matchers::WithinAbs(0., 1e-9));
    REQUIRE_FALSE(print.wipe_tower_data().tool_changes.empty());
}

TEST_CASE("Added purge walls obey the support rule across material regions", "[PurgeInnerWalls]")
{
    Print print;
    Model model;
    init_two_color_cube(print, model, purge_config("classic", 30.));
    print.process();
    bool found_extra = false;
    for (const Layer *layer : print.get_object(0)->layers()) {
        if (!layer->lower_layer) continue;
        int upper_count = 0, lower_count = 0;
        for (const LayerRegion *region : layer->regions())
            upper_count = std::max(upper_count, region->purge_inner_wall_plan.extra_loops);
        for (const LayerRegion *region : layer->lower_layer->regions())
            lower_count = std::max(lower_count, region->purge_inner_wall_plan.extra_loops);
        found_extra |= upper_count > 0;
        REQUIRE(lower_count >= upper_count - 1);
        Polygons below;
        for (const auto *region : layer->lower_layer->regions())
            region->perimeters.polygons_covered_by_width(below, float(SCALED_EPSILON));
        for (const auto *region : layer->regions())
            for (const auto *loop : region->perimeters.purge_inner_wall_candidates())
                if (loop->purge_safe)
                    REQUIRE(diff_pl(loop->as_polylines(), below).empty());
    }
    REQUIRE(found_extra);
}

TEST_CASE("Changing the purge wall option invalidates geometry and removes added loops", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    config.set_deserialize_strict({{"flush_into_inner_walls", false}});
    print.apply(model, config);
    REQUIRE_FALSE(print.get_object(0)->is_step_done(posPerimeters));
    REQUIRE_FALSE(print.get_object(0)->is_step_done(posPrepareInfill));
    REQUIRE_FALSE(print.get_object(0)->is_step_done(posInfill));
    REQUIRE_FALSE(print.is_step_done(psWipeTower));
    print.process();
    for (const auto *layer : print.get_object(0)->layers())
        for (const auto *region : layer->regions()) {
            REQUIRE(region->purge_inner_wall_plan.extra_loops == 0);
            REQUIRE(region->perimeters.purge_inner_wall_candidates().empty());
        }
}

TEST_CASE("Dedicated purge objects are consumed before extra walls on other objects", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 5.);
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    auto *purge = model.add_object();
    purge->add_volume(make_cube(30., 30., 20.));
    purge->add_instance()->set_offset(Vec3d(50., 50., 0.));
    purge->config.set_key_value("flush_into_objects", new ConfigOptionBool(true));
    purge->config.set_key_value("flush_into_inner_walls", new ConfigOptionBool(false));
    print.apply(model, config);
    print.process();
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.requested > 0.);
    REQUIRE(stats.other > 0.);
    REQUIRE_THAT(stats.inner_walls, Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_THAT(stats.reduced, Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Soluble transitions are not assigned to model purge walls", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    config.set_deserialize_strict("filament_soluble", "0,1");
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.requested > 0.);
    REQUIRE_THAT(stats.inner_walls, Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_THAT(stats.reduced, Catch::Matchers::WithinAbs(stats.requested, 1e-6));
}

TEST_CASE("Purge wall capacity accounts for the incoming filament flow ratio", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    config.set_deserialize_strict("filament_flow_ratio", "0.5,0.5");
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.inner_walls > 0.);
    REQUIRE_THAT(stats.reduced, Catch::Matchers::WithinAbs(0., 1e-5));
}

TEST_CASE("Sequential copies allocate purge only to the current copy", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    config.set_deserialize_strict("print_sequence", "by object");
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    auto *object = model.objects.front();
    auto *copy = object->add_instance(*object->instances.front());
    copy->set_offset(copy->get_offset() + Vec3d(70., 0., 0.));
    print.apply(model, config);
    const auto output = gcode(print);
    // Both copies have their own interior 1 -> 2 transition. The between-copy
    // 2 -> 1 change is allocated on the new copy's first layer, which has no
    // hidden capacity, rather than silently purging in the load macro.
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.inner_walls >= 2. * 15. - 1e-5);
    REQUIRE(stats.reduced >= 15. - 1e-5);
    REQUIRE(output.find("; PURGE INNER WALLS") != std::string::npos);
}

TEST_CASE("Fuzzy skin preserves exterior texture while added purge walls remain repeatable", "[PurgeInnerWalls]")
{
    const char *generator = GENERATE("classic", "arachne");
    const char *fuzzy_mode = GENERATE("external", "allwalls");
    CAPTURE(generator, fuzzy_mode);
    auto config = purge_config(generator, 15.);
    config.set_deserialize_strict({{"fuzzy_skin", fuzzy_mode}, {"fuzzy_skin_thickness", 0.3}, {"fuzzy_skin_point_distance", 0.4}});
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    REQUIRE(print.inner_wall_purge_statistics().inner_walls > 0.);
    REQUIRE_THAT(print.inner_wall_purge_statistics().reduced, Catch::Matchers::WithinAbs(0., 1e-5));
    bool textured_exterior = false;
    Layer *trial_layer = nullptr;
    LayerRegion *trial_region = nullptr;
    for (Layer *layer : print.get_object(0)->layers())
        for (LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntity *entity : region->perimeters.flatten().entities)
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                    size_t points = 0;
                    for (const auto &path : loop->paths) points += path.polyline.points.size();
                    if (loop->inset_idx == 0 && points > 20) {
                        REQUIRE_FALSE(loop->generated_for_purge);
                        REQUIRE_FALSE(loop->purge_support);
                        textured_exterior = true;
                    }
                }
            if (!region->perimeters.purge_inner_wall_candidates().empty()) {
                trial_layer = layer;
                trial_region = region;
            }
        }
    REQUIRE(textured_exterior);
    REQUIRE(trial_region != nullptr);
    std::vector<double> volumes;
    for (const auto *loop : trial_region->perimeters.purge_inner_wall_candidates()) volumes.push_back(loop->total_volume());
    // A different random texture realization must not change measured purge
    // capacity, even when the user's ordinary inner walls have fuzzy skin.
    trial_layer->make_perimeters();
    const auto regenerated = trial_region->perimeters.purge_inner_wall_candidates();
    REQUIRE(regenerated.size() == volumes.size());
    for (size_t i = 0; i < volumes.size(); ++i)
        REQUIRE_THAT(regenerated[i]->total_volume(), Catch::Matchers::WithinAbs(volumes[i], 1e-6));
}

TEST_CASE("Z contouring remains active on a sloped roof beside planar purge walls", "[PurgeInnerWalls]")
{
    const char *generator = GENERATE("classic", "arachne");
    auto config = purge_config(generator, 15.);
    config.set_deserialize_strict({{"zaa_enabled", true}, {"zaa_min_z", 0.1}});
    // Flat base for support, genuinely sloped/non-layer-aligned roof so this
    // checks contouring output, not merely acceptance of an enabled checkbox.
    auto mesh = make_cube(20., 20., 20.).its;
    for (auto &vertex : mesh.vertices)
        if (vertex.z() > 10.f) vertex.z() += 0.23f + 0.3f * vertex.x();
    Print print;
    Model model;
    init_two_color_cube(print, model, config, TriangleMesh(std::move(mesh)));
    print.process();
    REQUIRE(print.inner_wall_purge_statistics().inner_walls > 0.);
    bool contoured_surface = false;
    bool added_wall = false;
    for (const Layer *layer : print.get_object(0)->layers())
        for (const LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntity *entity : region->perimeters.flatten().entities)
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                    added_wall |= loop->generated_for_purge;
                    for (const auto &path : loop->paths) {
                        if (loop->generated_for_purge || loop->purge_support) {
                            REQUIRE_FALSE(path.z_contoured);
                            for (const Point3 &point : path.polyline.points) REQUIRE(point.z() == 0);
                        } else contoured_surface |= path.z_contoured;
                    }
                }
            for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) contoured_surface |= path->z_contoured;
        }
    REQUIRE(added_wall);
    REQUIRE(contoured_surface);
}

TEST_CASE("Dedicated purge extrusion precedes additional model walls", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    auto *purge = model.add_object();
    purge->add_volume(make_cube(6., 6., 20.));
    purge->add_instance()->set_offset(Vec3d(50., 50., 0.));
    purge->config.set_key_value("flush_into_objects", new ConfigOptionBool(true));
    purge->config.set_key_value("flush_into_inner_walls", new ConfigOptionBool(false));
    print.apply(model, config);
    const auto output = gcode(print);
    const auto stats = print.inner_wall_purge_statistics();
    REQUIRE(stats.other > 0.);
    REQUIRE(stats.inner_walls > 0.);
    const auto objects = output.find("; PURGE OBJECTS");
    REQUIRE(objects != std::string::npos);
    const auto walls = output.find("; PURGE INNER WALLS", objects);
    REQUIRE(walls != std::string::npos);
    const auto dirty_object_wall = output.find("; FEATURE: Outer wall", objects);
    REQUIRE(dirty_object_wall != std::string::npos);
    REQUIRE(dirty_object_wall < walls);
}

TEST_CASE("Changing filament flow replans purge geometry instead of only exporting again", "[PurgeInnerWalls]")
{
    auto config = purge_config("classic", 15.);
    Print print;
    Model model;
    init_two_color_cube(print, model, config);
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    config.set_deserialize_strict("filament_flow_ratio", "0.5,0.5");
    print.apply(model, config);
    REQUIRE_FALSE(print.is_step_done(psWipeTower));
    print.process();
    REQUIRE_THAT(print.inner_wall_purge_statistics().reduced, Catch::Matchers::WithinAbs(0., 1e-5));
}
