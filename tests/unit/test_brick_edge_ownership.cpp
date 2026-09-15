#include <doctest/doctest.h>

#include "../../src/mesh/brick_edge_ownership.h"

#include <array>
#include <limits>
#include <vector>

using namespace clay;

namespace {

bool contains_edge(const std::array<int, 3>& a, const std::array<int, 3>& b,
                   const std::array<int, 3>& low, int dim) {
    for (int axis = 0; axis < 3; ++axis)
        if (a[axis] < low[axis] || a[axis] > low[axis] + dim ||
            b[axis] < low[axis] || b[axis] > low[axis] + dim) return false;
    return true;
}

bool shared_with_neighbor(const std::array<int, 3>& a, const std::array<int, 3>& b,
                          brick::BrickKey owner, int dim) {
    for (int neighbor = 0; neighbor < 27; ++neighbor) {
        if (neighbor == 13) continue;
        const std::array<int, 3> low{
            (owner.x + neighbor % 3 - 1) * dim,
            (owner.y + neighbor / 3 % 3 - 1) * dim,
            (owner.z + neighbor / 9 - 1) * dim};
        if (contains_edge(a, b, low, dim)) return true;
    }
    return false;
}

void check_edges(int dim, brick::BrickKey key) {
    const int side = dim + 1;
    for (int point = 0; point < side * side * side; ++point) {
        const std::array<int, 3> a{point % side + key.x * dim,
                                   point / side % side + key.y * dim,
                                   point / side / side + key.z * dim};
        for (int direction = 1; direction < 8; ++direction) {
            const std::array<int, 3> b{a[0] + (direction & 1),
                                       a[1] + ((direction >> 1) & 1),
                                       a[2] + ((direction >> 2) & 1)};
            if (!contains_edge(a, b, {key.x * dim, key.y * dim, key.z * dim}, dim))
                continue;
            const bool shared = shared_with_neighbor(a, b, key, dim);
            CHECK(mesh::detail::interior_edge(a, b, key, dim) == !shared);
            CHECK(mesh::detail::interior_edge(b, a, key, dim) == !shared);
        }
    }
}

}  // namespace

TEST_CASE("exclusive edge classification matches neighboring brick containment") {
    for (int dim = 1; dim <= 16; ++dim)
        for (const brick::BrickKey key : {brick::BrickKey{0, 0, 0}, {-3, 2, -1}, {5, -4, 7}})
            check_edges(dim, key);
}

TEST_CASE("exclusive welding eligibility rejects duplicates and packed coordinate aliases") {
    using mesh::detail::unique_bounded_bricks;
    const std::vector<brick::BrickKey> ordinary{{-3, 2, -1}, {0, 0, 0}, {4, -2, 1}};
    for (int dim = 1; dim <= 16; ++dim) CHECK(unique_bounded_bricks(ordinary, dim));
    for (int dim : {-1, 0, 17, 32}) CHECK_FALSE(unique_bounded_bricks(ordinary, dim));
    auto repeated = ordinary;
    repeated.push_back(ordinary.front());
    CHECK_FALSE(unique_bounded_bricks(repeated, 8));
    constexpr int limit = 1 << 20;
    const std::vector<brick::BrickKey> valid_extremes{{-limit, 0, 0}, {limit - 2, 0, 0}};
    CHECK(unique_bounded_bricks(valid_extremes, 1));
    for (int coordinate : {-limit - 1, limit - 1, std::numeric_limits<int>::min(),
                           std::numeric_limits<int>::max()}) {
        const std::vector<brick::BrickKey> x{{coordinate, 0, 0}};
        const std::vector<brick::BrickKey> y{{0, coordinate, 0}};
        const std::vector<brick::BrickKey> z{{0, 0, coordinate}};
        CHECK_FALSE(unique_bounded_bricks(x, 1));
        CHECK_FALSE(unique_bounded_bricks(y, 1));
        CHECK_FALSE(unique_bounded_bricks(z, 1));
        CHECK_FALSE(unique_bounded_bricks(x, 16));
    }
}
