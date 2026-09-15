#include <doctest/doctest.h>

#include "../../src/mesh/edge_welding.h"

#include <array>
#include <limits>
#include <unordered_map>

namespace {
using clay::mesh::detail::EdgeKey;
using clay::mesh::detail::EdgeKeyHash;
using clay::mesh::detail::EdgeVertexMap;

struct ConstantHash {
    std::size_t operator()(EdgeKey) const { return 63; }
};

void check_result(std::pair<std::uint32_t, bool> actual,
                  std::pair<std::uint32_t, bool> expected) {
    CHECK(actual.first == expected.first);
    CHECK(actual.second == expected.second);
}

template <class Hash>
void check_growth() {
    EdgeVertexMap<Hash> actual;
    std::unordered_map<EdgeKey, std::uint32_t, EdgeKeyHash> expected;
    for (std::uint32_t i = 0; i < 2048; ++i) {
        const EdgeKey key{std::uint64_t(i) << 42, std::uint64_t(i) << 21};
        const auto [entry, inserted] = expected.emplace(key, i * 3);
        const auto result = actual.intern(key, i * 3);
        check_result(result, {entry->second, inserted});
        // Repeated keys must retain their original value after every growth.
        const auto repeated = actual.intern({0, 0}, 999);
        check_result(repeated, {0, false});
    }
    for (const auto& [key, value] : expected) {
        const auto result = actual.intern(key, 0);
        check_result(result, {value, false});
    }
}
}  // namespace

TEST_CASE("edge welding preserves first indices through growth and collisions") {
    SUBCASE("mixed packed-coordinate hashes") { check_growth<EdgeKeyHash>(); }
    SUBCASE("every hash collides across the bucket wrap") { check_growth<ConstantHash>(); }
}

TEST_CASE("edge welding keeps all key and index bits and independent builders") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array<EdgeKey, 5> keys{{{0, 0}, {0, maximum}, {maximum, 0},
                                       {maximum, maximum}, {maximum - 1, maximum}}};
    EdgeVertexMap<ConstantHash> first, second;
    for (std::uint32_t i = 0; i < keys.size(); ++i) {
        const auto value = std::numeric_limits<std::uint32_t>::max() - i;
        check_result(first.intern(keys[i], value), {value, true});
        check_result(second.intern(keys[i], i), {i, true});
        check_result(first.intern(keys[i], i), {value, false});
        check_result(second.intern(keys[i], value), {i, false});
    }
}
