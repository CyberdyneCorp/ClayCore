#include "clay/field/volume.h"
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
using namespace clay;
using field::FieldVolume;
using kernel::cf3;
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::ofstream output(argv[1], std::ios::binary);
    auto write = [&](const auto &value) {
        output.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    std::puts("case,repeat,ms");
    for (int which = 0; which < 6; ++which) {
        for (int repeat = 0; repeat < 7; ++repeat) {
            const math::Aabb bounds{cf3(-1, -1, -1), cf3(1, 1, 1)};
            auto volume = FieldVolume::empty_lattice(bounds, .02f, .06f);
            std::vector<std::uint64_t> observed;
            const FieldVolume::BrickBlockFill fill = [&](const auto &, std::size_t first,
                                                         std::size_t count, float *values) {
                observed.push_back(volume.sample_count());
                observed.push_back(first);
                observed.push_back(count);
                for (std::size_t k = 0; k < count * field::kBrickSamples; ++k) {
                    const std::uint32_t bits =
                        0x3c000000u +
                        static_cast<std::uint32_t>((first * field::kBrickSamples + k) % 100003);
                    values[k] = std::bit_cast<float>(bits);
                }
            };
            std::vector<FieldVolume::BrickCoord> added;
            auto region = which % 2 == 0 ? FieldVolume::Region{bounds}
                                         : FieldVolume::Region::ball(cf3(0, 0, 0), .35f);
            if (which >= 2)
                volume.materialize_region(FieldVolume::Region::ball(cf3(-.6f, 0, 0), .2f), fill,
                                          &added);
            const auto start = std::chrono::steady_clock::now();
            auto tally = volume.materialize_region(region, fill, &added);
            if (which >= 4)
                tally = volume.materialize_region(region, fill, &added);
            const auto stop = std::chrono::steady_clock::now();
            std::printf("%d,%d,%.6f\n", which, repeat,
                        std::chrono::duration<double, std::milli>(stop - start).count());
            write(tally.evaluated);
            write(tally.kept);
            write(tally.added);
            const auto lip = volume.sample_lipschitz();
            write(lip);
            const auto blob = volume.to_blob();
            write(blob.size());
            output.write(reinterpret_cast<const char *>(blob.data()),
                         static_cast<std::streamsize>(blob.size() * sizeof(blob[0])));
            write(observed.size());
            for (auto v : observed)
                write(v);
            write(added.size());
            for (const auto &v : added) {
                write(v.x);
                write(v.y);
                write(v.z);
            }
        }
    }
    return output ? 0 : 3;
}
