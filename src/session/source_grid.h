#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "clay/field/volume.h"

namespace clay::session::detail {

// Decline before allocating or touching output. The old path holds three floats
// per brick sample; the unique path holds three coordinates plus one value.
inline std::optional<std::array<std::size_t, 3>>
unique_source_shape(const field::FieldVolume::BrickGrid &grid, std::size_t first,
                    std::size_t count) {
    constexpr std::size_t per = field::kBrickSamples;
    constexpr auto max_bytes = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (first != 0 || count < 64 || count > max_bytes / (per * 3 * sizeof(float)))
        return std::nullopt;
    std::array<std::size_t, 3> shape{};
    std::size_t bricks = 1;
    std::size_t samples = 1;
    const std::size_t max_samples = count * per * 3 / 4;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const int extent = grid.bcount[axis];
        if (extent <= 0 || extent > (std::numeric_limits<int>::max() - 1) / field::kBrickDim)
            return std::nullopt;
        const auto dim = static_cast<std::size_t>(extent);
        if (bricks > count / dim)
            return std::nullopt;
        bricks *= dim;
        shape[axis] = dim * field::kBrickDim + 1;
        if (samples > max_samples / shape[axis])
            return std::nullopt;
        samples *= shape[axis];
    }
    if (bricks != count)
        return std::nullopt;
    return shape;
}

// One evaluation per unique lattice position, followed by the existing brick layout.
template <class Fill>
bool fill_unique_source_grid(const field::FieldVolume::BrickGrid &grid, std::size_t first,
                             std::size_t count, float *out, Fill &&fill) {
    const auto shape = unique_source_shape(grid, first, count);
    if (!shape)
        return false;
    const auto [nx, ny, nz] = *shape;
    const std::size_t unique = nx * ny * nz;
    const auto bx_count = static_cast<std::size_t>(grid.bcount[0]);
    const auto by_count = static_cast<std::size_t>(grid.bcount[1]);
    constexpr auto dim = field::kBrickDim;
    constexpr auto n = dim + 1;
    std::vector<float> points(unique * 3);
    std::vector<float> values(unique);
    std::size_t at = 0;
    for (std::size_t z = 0; z < nz; ++z)
        for (std::size_t y = 0; y < ny; ++y)
            for (std::size_t x = 0; x < nx; ++x) {
                const auto p =
                    grid.origin + kernel::cf3(static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(z)) *
                                      grid.cell_size;
                points[at++] = p.x;
                points[at++] = p.y;
                points[at++] = p.z;
            }
    fill(points.data(), unique, values.data());
    for (std::size_t brick = 0; brick < count; ++brick) {
        const auto bx = brick % bx_count;
        const auto by = brick / bx_count % by_count;
        const auto bz = brick / (bx_count * by_count);
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y) {
                const auto source = ((bz * dim + z) * ny + by * dim + y) * nx + bx * dim;
                const auto target = brick * field::kBrickSamples + (z * n + y) * n;
                std::copy_n(values.data() + source, n, out + target);
            }
    }
    return true;
}

} // namespace clay::session::detail
