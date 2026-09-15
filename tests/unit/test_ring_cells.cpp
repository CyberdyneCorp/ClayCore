#include <doctest/doctest.h>

#include "../../src/mesh/ring_cells.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
using Point = std::array<int, 3>;
using clay::mesh::detail::RingNeighbors;

bool touches_requested_box(Point cell, int dim, const RingNeighbors& wanted) {
    for (int neighbor = 0; neighbor < 27; ++neighbor) {
        if (!wanted[neighbor]) continue;
        const Point low{(neighbor % 3 - 1) * dim, (neighbor / 3 % 3 - 1) * dim,
                        (neighbor / 9 - 1) * dim};
        bool overlaps = true;
        for (int axis = 0; axis < 3; ++axis)
            overlaps = overlaps && cell[axis] <= low[axis] + dim &&
                       cell[axis] + 1 >= low[axis];
        if (overlaps) return true;
    }
    return false;
}

void check_cells(int dim, const RingNeighbors& wanted) {
    std::vector<Point> expected;
    for (int z = 0; z < dim; ++z)
        for (int y = 0; y < dim; ++y)
            for (int x = 0; x < dim; ++x)
                if (touches_requested_box({x, y, z}, dim, wanted))
                    expected.push_back({x, y, z});
    std::vector<Point> actual;
    clay::mesh::detail::for_each_ring_cell(dim, wanted, [&](int x, int y, int z) {
        actual.push_back({x, y, z});
    });
    CHECK(actual == expected);
}

void check_masks(int dim) {
    CAPTURE(dim);
    RingNeighbors wanted{};
    check_cells(dim, wanted);
    wanted.fill(true);
    wanted[13] = false;
    check_cells(dim, wanted);
    for (int neighbor = 0; neighbor < 27; ++neighbor) {
        if (neighbor == 13) continue;
        CAPTURE(neighbor);
        wanted.fill(false);
        wanted[neighbor] = true;
        check_cells(dim, wanted);
    }
    std::uint32_t random = 0x729ab83u;
    for (int mask = 0; mask < 32; ++mask) {
        for (bool& requested : wanted) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            requested = (random & 1u) != 0;
        }
        wanted[13] = false;
        check_cells(dim, wanted);
    }
}
}  // namespace

TEST_CASE("ring cell rows match independent closed-box intersections") {
    for (int dim = 1; dim <= 16; ++dim) check_masks(dim);
    check_masks(32);
    check_masks(0);
    check_masks(-1);
}
