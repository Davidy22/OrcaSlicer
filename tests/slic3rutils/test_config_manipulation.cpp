#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ConfigManipulation.hpp"

#include <boost/any.hpp>
#include <string>
#include <utility>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
DynamicPrintConfig purge_options(bool inner_walls, bool tower)
{
    DynamicPrintConfig config;
    config.set_key_value("purge_into_inner_walls", new ConfigOptionBool(inner_walls));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(tower));
    config.set_key_value("purge_into_inner_walls_preferred", new ConfigOptionBool(true));
    return config;
}
} // namespace

TEST_CASE("Selecting a purge destination unchecks the other without another change event", "[ConfigManipulation][PurgeIntoInnerWalls][Regression]")
{
    const std::string selected = GENERATE(std::string("purge_into_inner_walls"), std::string("enable_prime_tower"));
    // The field has already stored the user's click when on_value_change runs.
    auto config = purge_options(true, true);
    int reloads = 0;
    int change_events = 0;
    ConfigManipulation* active = nullptr;
    ConfigManipulation manipulation(
        [&] {
            REQUIRE(++reloads == 1);
            // Tab reload/update also applies the default preset-conflict rule.
            // It must not undo the explicit choice or recurse indefinitely.
            active->update_purge_options(&config);
        }, {}, {}, [&](const std::string&, const boost::any&) { ++change_events; });
    active = &manipulation;

    manipulation.update_purge_options(&config, selected);
    CHECK(config.opt_bool("purge_into_inner_walls") == (selected == "purge_into_inner_walls"));
    CHECK(config.opt_bool("enable_prime_tower") == (selected == "enable_prime_tower"));
    CHECK(config.opt_bool("purge_into_inner_walls_preferred"));
    CHECK(reloads == 1);
    CHECK(change_events == 0);
    CHECK(manipulation.applying_keys().empty());

    manipulation.update_purge_options(&config, selected);
    CHECK(reloads == 1);
}

TEST_CASE("Purge options that do not conflict are left unchanged", "[ConfigManipulation][PurgeIntoInnerWalls]")
{
    const auto [inner_walls, tower] = GENERATE(std::make_pair(false, false),
                                              std::make_pair(true, false),
                                              std::make_pair(false, true));
    const std::string selected = GENERATE(std::string("purge_into_inner_walls"), std::string("enable_prime_tower"));
    auto config = purge_options(inner_walls, tower);
    int reloads = 0;
    ConfigManipulation manipulation([&] { ++reloads; }, {}, {}, {});

    manipulation.update_purge_options(&config, selected);
    CHECK(config.opt_bool("purge_into_inner_walls") == inner_walls);
    CHECK(config.opt_bool("enable_prime_tower") == tower);
    CHECK(reloads == 0);
}

TEST_CASE("Loaded conflicting purge options use the slicer's inner-wall priority", "[ConfigManipulation][PurgeIntoInnerWalls][Regression]")
{
    auto config = purge_options(true, true);
    int reloads = 0;
    ConfigManipulation manipulation([&] { ++reloads; }, {}, {}, {});
    manipulation.update_purge_options(&config);
    CHECK(config.opt_bool("purge_into_inner_walls"));
    CHECK_FALSE(config.opt_bool("enable_prime_tower"));
    CHECK(reloads == 1);

    auto normalized = purge_options(true, true);
    normalized.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));
    normalized.normalize_fdm_2(1, 2);
    CHECK(config.opt_bool("purge_into_inner_walls") == normalized.opt_bool("purge_into_inner_walls"));
    CHECK(config.opt_bool("enable_prime_tower") == normalized.opt_bool("enable_prime_tower"));
}

TEST_CASE("Purge-option updates do not add absent global options", "[ConfigManipulation][PurgeIntoInnerWalls]")
{
    const std::string present = GENERATE(std::string("purge_into_inner_walls"), std::string("enable_prime_tower"));
    DynamicPrintConfig config;
    config.set_key_value(present, new ConfigOptionBool(true));
    int reloads = 0;
    ConfigManipulation manipulation([&] { ++reloads; }, {}, {}, {});
    manipulation.update_purge_options(&config, present);
    manipulation.update_purge_options(&config);
    CHECK(config.keys().size() == 1);
    CHECK(config.opt_bool(present));
    CHECK(reloads == 0);
}

TEST_CASE("Unrelated option edits do not choose a purge destination", "[ConfigManipulation][PurgeIntoInnerWalls]")
{
    auto config = purge_options(true, true);
    int reloads = 0;
    ConfigManipulation manipulation([&] { ++reloads; }, {}, {}, {});
    manipulation.update_purge_options(&config, "enable_support");
    CHECK(config.opt_bool("purge_into_inner_walls"));
    CHECK(config.opt_bool("enable_prime_tower"));
    CHECK(reloads == 0);
}
