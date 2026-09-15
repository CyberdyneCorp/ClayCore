// Exact scalar/bulk coordinate comparison. Timings are informational.
#include "clay/field/volume.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using clay::field::FieldVolume;

void scalar_positions(const FieldVolume::BrickGrid& grid, std::size_t count, float* out) {
    for (std::size_t slot = 0; slot < count; ++slot)
        for (int i = 0; i < clay::field::kBrickSamples; ++i) {
            const auto p = grid.sample_position(slot, i);
            const std::size_t at = (slot * clay::field::kBrickSamples + i) * 3;
            out[at] = p.x;
            out[at + 1] = p.y;
            out[at + 2] = p.z;
        }
}

bool same_bits(const std::vector<float>& actual, const std::vector<float>& expected) {
    for (std::size_t i = 0; i < actual.size(); ++i)
        if (std::bit_cast<std::uint32_t>(actual[i]) !=
            std::bit_cast<std::uint32_t>(expected[i])) return false;
    return true;
}

bool measure_grid(float origin, float cell) {
    const FieldVolume::BrickGrid grid{
        clay::kernel::cf3(origin, origin + 0.17f, -origin), cell, 3.0f, {14, 14, 14}};
    constexpr std::size_t count = 14 * 14 * 14;
    std::vector<float> expected(count * clay::field::kBrickSamples * 3);
    std::vector<float> actual(expected.size());
    scalar_positions(grid, count, expected.data());
    for (int run = 0; run < 7; ++run)
        for (int order = 0; order < 2; ++order) {
            const bool bulk = (run + order) % 2 != 0;
            const auto started = std::chrono::steady_clock::now();
            if (bulk) grid.sample_positions(0, count, actual.data());
            else scalar_positions(grid, count, actual.data());
            const double millis = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (!same_bits(actual, expected)) {
                std::fprintf(stderr, "Coordinate mismatch: origin=%g cell=%g run=%d\n",
                             origin, cell, run);
                return false;
            }
            std::printf("%g,%g,%zu,%d,%s,%.6f\n", origin, cell, count, run,
                        bulk ? "bulk" : "scalar", millis);
        }
    return true;
}
}  // namespace

int main() {
    std::puts("origin,cell,bricks,run,variant,milliseconds");
    for (float origin : {0.0f, -1.24f, 50.0f, 1e9f})
        for (float cell : {0.02f, 0.013f})
            if (!measure_grid(origin, cell)) return 1;
    return 0;
}
