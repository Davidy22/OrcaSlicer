#include <catch2/catch_all.hpp>

#include "libslic3r/PurgeInnerWallPlan.hpp"

#include <limits>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("One large loop cannot absorb two separate transitions", "[PurgeInnerWallPlan]")
{
    REQUIRE_THAT(purge_inner_wall_shortfall({5., 5.}, {100.}), WithinAbs(5., 1e-9));
    REQUIRE_THAT(purge_inner_wall_shortfall({5., 5.}, {100., 5.}), WithinAbs(0., 1e-9));
}

TEST_CASE("Whole-loop shortfall preserves per-transition demand and zero transitions", "[PurgeInnerWallPlan]")
{
    REQUIRE_THAT(purge_inner_wall_shortfall({0., 10., 0., 20.}, {5., 5., 7.}), WithinAbs(13., 1e-9));
    REQUIRE_THAT(purge_inner_wall_shortfall({5., 7.}, {}), WithinAbs(12., 1e-9));
    REQUIRE_THAT(purge_inner_wall_shortfall({}, {10.}), WithinAbs(0., 1e-9));
    REQUIRE_THROWS_AS(purge_inner_wall_shortfall({-1.}, {10.}), std::invalid_argument);
    REQUIRE_THROWS_AS(purge_inner_wall_shortfall({1.}, {std::numeric_limits<double>::quiet_NaN()}), std::invalid_argument);
}

TEST_CASE("Whole-loop capacity order cannot be rearranged to hide overflow", "[PurgeInnerWallPlan]")
{
    REQUIRE_THAT(purge_inner_wall_shortfall({1., 100.}, {100., 1.}), WithinAbs(99., 1e-9));
    REQUIRE_THAT(purge_inner_wall_shortfall({1., 100.}, {1., 100.}), WithinAbs(0., 1e-9));
}

TEST_CASE("Invalid measured purge budgets fail closed", "[PurgeInnerWallPlan]")
{
    const double invalid = GENERATE(-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
    REQUIRE_THROWS_AS(purge_inner_wall_shortfall({invalid}, {10.}), std::invalid_argument);
    REQUIRE_THROWS_AS(purge_inner_wall_shortfall({10.}, {invalid}), std::invalid_argument);
}

TEST_CASE("Whole-loop budget accumulation cannot overflow silently", "[PurgeInnerWallPlan]")
{
    const double maximum = std::numeric_limits<double>::max();
    REQUIRE_THROWS_AS(purge_inner_wall_shortfall({maximum, maximum}, {}), std::invalid_argument);
}

TEST_CASE("Zero-volume candidates cannot absorb purge or stall allocation", "[PurgeInnerWallPlan]")
{
    REQUIRE_THAT(purge_inner_wall_shortfall({10.}, {0., 0., 4.}), WithinAbs(6., 1e-9));
    REQUIRE_THAT(purge_inner_wall_shortfall({0., 4.}, {0., 4.}), WithinAbs(0., 1e-9));
}
