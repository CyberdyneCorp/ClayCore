#include <doctest/doctest.h>

#include "../../src/field/sample_bounds.h"

#include <array>
#include <bit>
#include <cstdint>

namespace {
constexpr int n = clay::field::kBrickDim + 1;
using Block = std::array<float, clay::field::kBrickSamples>;
constexpr std::array<int, 3> strides{1, n, n * n};

float scalar_bound(const Block& block) {
    float maximum = 0.0f;
    for (int at = 0; at < clay::field::kBrickSamples; ++at) {
        for (int stride : strides) {
            if ((at / stride) % n == n - 1) continue;
            maximum = std::max(maximum, std::abs(block[at + stride] - block[at]));
        }
    }
    return maximum;
}

void check_bound(const Block& block) {
    const auto expected = std::bit_cast<std::uint32_t>(scalar_bound(block));
    const auto actual =
        std::bit_cast<std::uint32_t>(clay::field::detail::steepest_in_block(block.data()));
    CHECK(actual == expected);
}

std::uint32_t next_bits(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
}  // namespace

TEST_CASE("sample_bounds: every forward pair can determine the maximum") {
    Block block{};
    for (int at = 0; at < clay::field::kBrickSamples; ++at) {
        for (int stride : strides) {
            if ((at / stride) % n == n - 1) continue;
            // Only this pair has difference two; every other difference is
            // zero or one. This detects omitted pairs, including row tails.
            block[at] = -1.0f;
            block[at + stride] = 1.0f;
            CHECK(clay::field::detail::steepest_in_block(block.data()) == 2.0f);
            block[at] = block[at + stride] = 0.0f;
        }
    }
}

TEST_CASE("sample_bounds: finite and special values match an independent oracle") {
    std::uint32_t state = 123456789;
    Block block{};
    for (int fixture = 0; fixture < 2048; ++fixture) {
        for (float& value : block) {
            const auto bits = next_bits(state);
            value = fixture % 2 != 0
                        ? std::bit_cast<float>(bits)
                        : (static_cast<float>(bits % 20001) - 10000.0f) / 100000.0f;
        }
        check_bound(block);
    }
    constexpr std::array<std::uint32_t, 10> special{
        0, 0x80000000u, 0x7fc01234u, 0x7f812345u, 0x7f800000u,
        0xff800000u, 1, 0x80000001u, 0x7f7fffffu, 0xff7fffffu};
    for (auto first : special) {
        for (auto second : special) {
            for (int at = 0; at < clay::field::kBrickSamples; ++at)
                block[at] = std::bit_cast<float>(at % 2 != 0 ? first : second);
            check_bound(block);
        }
    }
}
