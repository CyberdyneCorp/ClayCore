#include <doctest/doctest.h>

#include "../../src/mesh/brick_samples.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using clay::mesh::detail::BrickSamples;

std::uint32_t sample_bits(int x, int y, int z) {
    // Include signed zero, infinities and NaN payloads, as well as ordinary
    // values. Coordinates select bits without floating-point arithmetic.
    constexpr std::array<std::uint32_t, 8> words{
        0, 0x80000000, 0x7f800000, 0xff800000,
        0x7fc00001, 0x7fa00002, 0x3f800000, 0xbf000000};
    const auto key = std::uint32_t(x) * 31u + std::uint32_t(y) * 7u + std::uint32_t(z);
    return words[key % words.size()];
}

void check_samples(int dimension, const std::array<int, 3>& low) {
    const int side = dimension + 1;
    std::vector<unsigned> visits(side * side * side);
    const BrickSamples samples(dimension, low.data(), [&](int x, int y, int z) {
        const int at = ((z - low[2]) * side + (y - low[1])) * side + (x - low[0]);
        REQUIRE(at >= 0);
        REQUIRE(static_cast<std::size_t>(at) < visits.size());
        ++visits[at];
        return std::bit_cast<float>(sample_bits(x, y, z));
    });
    for (auto count : visits) CHECK(count == 1);
    // Read in reverse order and repeatedly: lookup must not call the source.
    for (int z = dimension; z >= 0; --z)
        for (int y = dimension; y >= 0; --y)
            for (int x = dimension; x >= 0; --x) {
                const int i = low[0] + x, j = low[1] + y, k = low[2] + z;
                CHECK(std::bit_cast<std::uint32_t>(samples(i, j, k)) == sample_bits(i, j, k));
                CHECK(std::bit_cast<std::uint32_t>(samples(i, j, k)) == sample_bits(i, j, k));
            }
    for (auto count : visits) CHECK(count == 1);
}
}  // namespace

TEST_CASE("brick samples preserve exact bits and read each lattice point once") {
    for (int dimension = 1; dimension <= BrickSamples::max_dimension; ++dimension) {
        CAPTURE(dimension);
        check_samples(dimension, {-19, 7, -3});
    }
    check_samples(16, {std::numeric_limits<int>::min(),
                       std::numeric_limits<int>::max() - 16, 0});
}

TEST_CASE("brick sample blocks have bounded independent storage") {
    static_assert(BrickSamples::max_samples * sizeof(float) == 19652);
    const int low[3]{-8, 0, 8};
    const BrickSamples first(8, low, [](int, int, int) { return -0.0f; });
    const BrickSamples second(8, low, [](int, int, int) { return 1.0f; });
    CHECK(std::bit_cast<std::uint32_t>(first(-8, 0, 8)) == 0x80000000u);
    CHECK(second(-8, 0, 8) == 1.0f);
    CHECK(std::bit_cast<std::uint32_t>(first(0, 8, 16)) == 0x80000000u);
    CHECK(second(0, 8, 16) == 1.0f);
}
