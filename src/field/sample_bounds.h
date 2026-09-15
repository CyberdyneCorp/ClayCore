#pragma once

#include "clay/field/volume.h"

#include <algorithm>
#include <cmath>

namespace clay::field::detail {

// Largest absolute difference across every forward-neighbor pair in a brick.
// Each accumulator starts at +0 and std::max ignores a NaN second operand,
// so all four remain nonnegative and non-NaN. Their final maximum therefore
// preserves the scalar result, including +0 and infinity. Independent lanes
// shorten the reduction dependency chain without changing float operations.
inline float steepest_in_block(const float* block) {
    constexpr int n = kBrickDim + 1;
    constexpr int kStride[3] = {1, n, n * n};
    float lanes[4] = {};
    for (int axis = 0; axis < 3; ++axis) {
        const int stride = kStride[axis];
        const int last[3] = {axis == 0 ? n - 1 : n, axis == 1 ? n - 1 : n, axis == 2 ? n - 1 : n};
        for (int z = 0; z < last[2]; ++z)
            for (int y = 0; y < last[1]; ++y) {
                const float* row = block + (z * n + y) * n;
                int x = 0;
                for (; x + 4 <= last[0]; x += 4) {
                    lanes[0] = std::max(lanes[0], std::abs(row[x + stride] - row[x]));
                    lanes[1] = std::max(lanes[1], std::abs(row[x + 1 + stride] - row[x + 1]));
                    lanes[2] = std::max(lanes[2], std::abs(row[x + 2 + stride] - row[x + 2]));
                    lanes[3] = std::max(lanes[3], std::abs(row[x + 3 + stride] - row[x + 3]));
                }
                for (; x < last[0]; ++x)
                    lanes[0] = std::max(lanes[0], std::abs(row[x + stride] - row[x]));
            }
    }
    return std::max(std::max(lanes[0], lanes[1]), std::max(lanes[2], lanes[3]));
}

}  // namespace clay::field::detail
