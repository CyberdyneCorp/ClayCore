#pragma once

#include "clay/brick/cache.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_set>

namespace clay::mesh::detail {

inline bool unique_bounded_bricks(std::span<const brick::BrickKey> keys, int dim) {
    if (dim < 1 || dim > 16) return false;
    std::unordered_set<brick::BrickKey, brick::BrickKeyHash> seen;
    seen.reserve(keys.size());
    constexpr std::int64_t limit = 1 << 20;
    for (const auto& key : keys) {
        for (int coordinate : {key.x, key.y, key.z}) {
            const auto low = static_cast<std::int64_t>(coordinate) * dim;
            if (low < -limit || low + dim >= limit) return false;
        }
        if (!seen.insert(key).second) return false;
    }
    return true;
}

// Preconditions: the request passed unique_bounded_bricks, and both endpoints
// belong to this ordinary brick. Straddlers must use global welding instead.
inline bool interior_edge(std::array<int, 3> a, std::array<int, 3> b,
                          brick::BrickKey key, int dim) {
    const int start[3] = {key.x * dim, key.y * dim, key.z * dim};
    for (int axis = 0; axis < 3; ++axis) {
        const int low = start[axis], high = low + dim;
        if ((a[axis] == low && b[axis] == low) ||
            (a[axis] == high && b[axis] == high)) return false;
    }
    return true;
}

}  // namespace clay::mesh::detail
