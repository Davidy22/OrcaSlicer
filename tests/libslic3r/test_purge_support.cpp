#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Support/PurgeSupport.hpp"
#include "libslic3r/libslic3r.h"

#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
ExPolygon support_box(double size = 20.)
{
    return ExPolygon(Polygon(Points{Point::new_scale(0., 0.), Point::new_scale(size, 0.),
                                    Point::new_scale(size, size), Point::new_scale(0., size)}));
}

class CloneCountedPath final : public ExtrusionPath
{
public:
    CloneCountedPath(const ExtrusionPath &path, size_t &clones) : ExtrusionPath(path), m_clones(clones) {}
    ExtrusionEntity *clone() const override
    {
        ++m_clones;
        return new CloneCountedPath(*this);
    }

private:
    size_t &m_clones;
};

ExtrusionEntityCollection sparse_support(const Flow &flow, size_t *clones = nullptr)
{
    ExtrusionEntityCollection paths;
    for (const double y : {5., 10., 15.}) {
        ExtrusionPath path(erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
        path.polyline = Polyline3(Polyline(Points{Point(scale_(0.1), scale_(y)), Point(scale_(19.9), scale_(y))}));
        if (clones != nullptr)
            paths.entities.emplace_back(new CloneCountedPath(path, *clones));
        else
            paths.append(std::move(path));
    }
    return paths;
}
} // namespace

TEST_CASE("Support wall growth replaces infill instead of overprinting it", "[PurgeSupport][PurgeIntoInnerWalls]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    ExtrusionEntityCollection paths = sparse_support(flow);
    const double before = paths.total_volume();
    PurgeSupportRegion region(paths, ExPolygons{support_box()}, flow, 0);
    CHECK_THAT(region.grow(0.), WithinAbs(0., 1e-8));
    const double added = region.grow(1.);
    REQUIRE(added > 0.);
    CHECK_THAT(paths.total_volume() - before, WithinAbs(added, 1e-6));
    CHECK(region.wall_count() == 1);
    CHECK(paths.no_sort);

    double remaining_infill = 0.;
    size_t infill_paths = 0;
    for_each_extrusion_leaf(paths, [&](const ExtrusionEntity &entity) {
        if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            ++infill_paths;
            remaining_infill += path->total_volume();
            CHECK_THAT(path->height, WithinAbs(flow.height(), 1e-6));
        }
    });
    CHECK(infill_paths == 3);
    CHECK(remaining_infill < before);
    CHECK(remaining_infill > 0.);
}

TEST_CASE("Support purge walls grow beyond two loops and stop at solid geometry", "[PurgeSupport][PurgeIntoInnerWalls]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    ExtrusionEntityCollection paths = sparse_support(flow);
    const double before = paths.total_volume();
    PurgeSupportRegion region(paths, ExPolygons{support_box()}, flow, 2);
    const double added = region.grow(10000.);
    REQUIRE(region.wall_count() > 2);
    CHECK(added > 0.);
    CHECK_THAT(paths.total_volume() - before, WithinAbs(added, 1e-6));
    // Geometry, not the UI's two-loop maximum, bounds the runtime count.
    CHECK(region.wall_count() < size_t(scale_(20.) / flow.scaled_spacing()));
    CHECK(paths.total_volume() < 20. * 20. * flow.height() * 1.1);
    const size_t count = region.wall_count();
    CHECK_THAT(region.grow(10000.), WithinAbs(0., 1e-6));
    CHECK(region.wall_count() == count);
}

TEST_CASE("Support growth respects holes and retains paths outside its safe region", "[PurgeSupport][PurgeIntoInnerWalls]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    ExPolygon area = support_box();
    Polygon hole(Points{Point(scale_(8.), scale_(8.)), Point(scale_(8.), scale_(12.)),
                        Point(scale_(12.), scale_(12.)), Point(scale_(12.), scale_(8.))});
    area.holes.push_back(hole);
    ExtrusionEntityCollection paths;
    // A height-modulated native path outside the growth footprint must survive.
    ExtrusionPath outside(erSupportMaterial, 0.03, 0.4f, 0.1f);
    outside.polyline = Polyline3(Polyline(Points{Point::new_scale(-5., 0.), Point::new_scale(-1., 0.)}));
    paths.append(outside);
    PurgeSupportRegion region(paths, ExPolygons{area}, flow, 0);
    REQUIRE(region.grow(10000.) > 0.);
    size_t preserved = 0;
    for_each_extrusion_path(paths, [&](const ExtrusionPath &path) {
        CHECK(intersection_pl(Polylines{path.polyline.to_polyline()}, ExPolygons{ExPolygon(hole)}).empty());
        if (path.first_point().x() < 0) {
            ++preserved;
            CHECK_THAT(path.height, WithinAbs(outside.height, 1e-6));
            CHECK_THAT(path.mm3_per_mm, WithinAbs(outside.mm3_per_mm, 1e-6));
            CHECK_THAT(path.length(), WithinAbs(outside.length(), 1e-6));
        }
    });
    CHECK(preserved == 1);
}

TEST_CASE("Replanning support purge restores native paths and does not accumulate loops", "[PurgeSupport][PurgeIntoInnerWalls]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    ExtrusionEntityCollection paths = sparse_support(flow);
    const double native_volume = paths.total_volume();
    const size_t native_count = paths.entities.size();
    PurgeSupportRegion region(paths, ExPolygons{support_box()}, flow, 1);
    REQUIRE(region.grow(10000.) > 0.);
    REQUIRE(region.wall_count() > 2);
    region.reset();
    CHECK(region.wall_count() == 1);
    CHECK(paths.entities.size() == native_count);
    CHECK_THAT(paths.total_volume(), WithinAbs(native_volume, 1e-6));
    const double first = region.grow(1.);
    const size_t first_count = region.wall_count();
    region.reset();
    CHECK_THAT(region.grow(1.), WithinAbs(first, 1e-6));
    CHECK(region.wall_count() == first_count);
}

TEST_CASE("Support leaf traversal preserves loop units and nested entity identities", "[PurgeSupport]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    ExtrusionEntityCollection nested = sparse_support(flow);
    const ExtrusionEntity *first = nested.entities.front();
    ExtrusionEntityCollection root;
    root.append(std::move(nested));
    extrusion_entities_append_loops(root.entities, Polygons{support_box().contour}, erSupportMaterial,
                                    flow.mm3_per_mm(), flow.width(), flow.height());
    std::vector<const ExtrusionEntity *> leaves;
    for_each_extrusion_leaf(root, [&](const ExtrusionEntity &entity) { leaves.push_back(&entity); });
    REQUIRE(leaves.size() == 4);
    CHECK(leaves.front() == first);
    CHECK(dynamic_cast<const ExtrusionLoop *>(leaves.back()) != nullptr);
}

TEST_CASE("Support regions that cannot grow do not copy native paths", "[PurgeSupport][Regression][Memory]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    size_t clones = 0;
    ExtrusionEntityCollection paths = sparse_support(flow, &clones);
    const ExtrusionEntitiesPtr original = paths.entities;
    // Only a subset of a support group may be safe for growth. This footprint
    // vanishes at the first wall offset, so the native paths must stay untouched.
    PurgeSupportRegion region(paths, ExPolygons{support_box(0.1)}, flow, 2);
    CHECK_THAT(region.grow(10000.), WithinAbs(0., 1e-8));
    CHECK(clones == 0);
    region.reset();
    CHECK(paths.entities == original);
    CHECK(clones == 0);
}

TEST_CASE("Support replanning transfers its saved native paths instead of copying them", "[PurgeSupport][Regression][Memory]")
{
    const Flow flow(0.45f, 0.2f, 0.4f);
    size_t clones = 0;
    ExtrusionEntityCollection paths = sparse_support(flow, &clones);
    const double native_volume = paths.total_volume();
    const size_t native_count = paths.entities.size();
    const ExtrusionEntitiesPtr native_paths = paths.entities;
    PurgeSupportRegion region(paths, ExPolygons{support_box()}, flow, 1);
    for (size_t iteration = 0; iteration < 16; ++iteration) {
        REQUIRE(region.grow(iteration % 2 == 0 ? 1. : 10000.) > 0.);
        CHECK(clones == 0);
        const size_t saved_clones = clones;
        region.reset();
        CHECK(clones == saved_clones);
        CHECK(paths.entities == native_paths);
        CHECK(paths.entities.size() == native_count);
        CHECK_THAT(paths.total_volume(), WithinAbs(native_volume, 1e-6));
        region.reset();
        CHECK(clones == saved_clones);
    }
}
