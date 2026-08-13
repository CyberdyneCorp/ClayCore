#include "occupancy_box.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace voxel {
namespace detail {

float OccupancyBox::trilinear(float fx, float fy, float fz) const {
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy)),
              z0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0),
                tz = fz - static_cast<float>(z0);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), tx);
    const float x10 = lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), tx);
    const float x01 = lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), tx);
    const float x11 = lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

bool build_occupancy(const VoxelGrid& grid, std::size_t level, int pad, std::uint8_t index,
                     OccupancyBox* out) {
    if (!out) return false;
    if (level >= grid.level_count()) return false;

    // The extent of the occupied CHUNKS, which is what the grid holds.
    const std::vector<VoxelCoord> keys = grid.occupied_chunk_keys(level);
    if (keys.empty()) return false;
    VoxelCoord lo = keys.front(), hi = keys.front();
    for (const VoxelCoord& k : keys) {
        lo = {std::min(lo.x, k.x), std::min(lo.y, k.y), std::min(lo.z, k.z)};
        hi = {std::max(hi.x, k.x), std::max(hi.y, k.y), std::max(hi.z, k.z)};
    }

    // Checked in 64-bit before anything is allocated. Material far apart on two
    // axes makes this box enormous while the material stays small, and a span
    // that overflowed an int would ask for a buffer no allocator can satisfy —
    // which ends the process, since this library builds without exceptions.
    long long total = 1;
    for (int a = 0; a < 3; ++a) {
        const long long l =
            static_cast<long long>(a == 0 ? lo.x : a == 1 ? lo.y : lo.z) * kChunkDim - pad;
        const long long h =
            (static_cast<long long>(a == 0 ? hi.x : a == 1 ? hi.y : hi.z) + 1) * kChunkDim - 1 +
            pad;
        const long long span = h - l + 1;
        if (span <= 0 || span > (1 << 20)) return false;
        out->min[a] = static_cast<int>(l);
        out->dim[a] = static_cast<int>(span);
        total *= span;
        if (total > (1LL << 28)) return false;  // ~256M samples, over a GiB of floats
    }

    out->value.assign(static_cast<std::size_t>(total), 0.0f);
    bool any = false;
    for (int z = out->min[2]; z < out->min[2] + out->dim[2]; ++z)
        for (int y = out->min[1]; y < out->min[1] + out->dim[1]; ++y)
            for (int x = out->min[0]; x < out->min[0] + out->dim[0]; ++x) {
                const std::uint8_t cell = grid.cell_index(level, {x, y, z});
                if (cell == 0) continue;
                if (index != 0 && cell != index) continue;
                out->value[out->index(x, y, z)] = 1.0f;
                any = true;
            }
    return any;
}

void blur_occupancy(OccupancyBox& box) {
    std::vector<float> scratch(box.value.size());
    const int x0 = box.min[0], y0 = box.min[1], z0 = box.min[2];
    const int x1 = x0 + box.dim[0], y1 = y0 + box.dim[1], z1 = z0 + box.dim[2];
    auto pass = [&](int dx, int dy, int dz) {
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    scratch[box.index(x, y, z)] = (box.at(x - dx, y - dy, z - dz) +
                                                   box.at(x, y, z) +
                                                   box.at(x + dx, y + dy, z + dz)) /
                                                  3.0f;
        box.value.swap(scratch);
    };
    pass(1, 0, 0);
    pass(0, 1, 0);
    pass(0, 0, 1);
}

}  // namespace detail
}  // namespace voxel
}  // namespace clay
