#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

using namespace Slic3r;

static_assert(std::is_nothrow_move_constructible_v<ExtrusionEntityCollection>);
static_assert(std::is_nothrow_move_assignable_v<ExtrusionEntityCollection>);

namespace {
class CountedPath final : public ExtrusionPath
{
public:
    explicit CountedPath(int &live) : ExtrusionPath(erPerimeter, 0.1, 0.45f, 0.2f), m_live(live)
    {
        ++m_live;
        polyline = Polyline3(Polyline(Points{Point::new_scale(0., 0.), Point::new_scale(10., 0.)}));
    }
    ~CountedPath() override { --m_live; }

private:
    int &m_live;
};

ExtrusionEntityCollection counted_collection(int &live)
{
    ExtrusionEntityCollection nested;
    nested.entities.emplace_back(new CountedPath(live));
    nested.entities.emplace_back(new CountedPath(live));
    ExtrusionEntityCollection root;
    root.append(std::move(nested));
    return root;
}
} // namespace

TEST_CASE("Move assignment releases an extrusion collection's previous owned entities", "[ExtrusionEntityCollection][Regression][Memory]")
{
    int live = 0;
    int most_live_after_assignment = 0;
    {
        ExtrusionEntityCollection destination = counted_collection(live);
        for (size_t iteration = 0; iteration < 1000; ++iteration) {
            ExtrusionEntityCollection source = counted_collection(live);
            source.no_sort = true;
            source.set_reverse();
            const ExtrusionEntity *incoming = source.entities.front();
            destination = std::move(source);
            most_live_after_assignment = std::max(most_live_after_assignment, live);
            REQUIRE(destination.entities.size() == 1);
            CHECK(destination.entities.front() == incoming);
            CHECK(source.empty());
            CHECK(destination.no_sort);
        }
        CHECK(live == 2);
        CHECK(most_live_after_assignment == 2);
        destination.no_sort = false;
        CHECK_FALSE(destination.can_reverse());
    }
    CHECK(live == 0);
}

TEST_CASE("Self move assignment preserves an extrusion collection's ownership", "[ExtrusionEntityCollection][Regression][Memory]")
{
    int live = 0;
    {
        ExtrusionEntityCollection collection = counted_collection(live);
        const ExtrusionEntity *original = collection.entities.front();
        auto *same_collection = &collection;
        collection = std::move(*same_collection);
        REQUIRE(collection.entities.size() == 1);
        CHECK(collection.entities.front() == original);
        CHECK(live == 2);
    }
    CHECK(live == 0);
}

TEST_CASE("Move assignment can replace a collection with its owned descendant", "[ExtrusionEntityCollection][Regression][Memory]")
{
    int live = 0;
    {
        ExtrusionEntityCollection collection = counted_collection(live);
        collection.entities.emplace_back(new CountedPath(live));
        auto &descendant = static_cast<ExtrusionEntityCollection &>(*collection.entities.front());
        descendant.set_reverse();
        const ExtrusionEntity *incoming = descendant.entities.front();
        collection = std::move(descendant);
        REQUIRE(collection.entities.size() == 2);
        CHECK(collection.entities.front() == incoming);
        CHECK_FALSE(collection.can_reverse());
        CHECK(live == 2);
    }
    CHECK(live == 0);
}
