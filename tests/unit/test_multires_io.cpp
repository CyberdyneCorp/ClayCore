// Persisting a hierarchy (file-io spec, add-mesh-multires).
//
// WHAT IS WRITTEN is the cage, the rule, the level count, which levels were
// active, and each level's detail — and nothing that follows from those. The
// per-level face lists and every evaluated position are derived, so writing
// them would create a second answer that a corrupt file could make disagree
// with the first.
//
// THE RULE IS RECORDED rather than assumed, and the depth is PRICED before it
// is built: a few hundred bytes can declare a twelve-level hierarchy over a
// large cage, which is a request for more memory than a machine holds, and the
// counts follow from the cage by arithmetic so the refusal costs nothing.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MultiresError;
using mesh::MultiresSurface;

namespace {

Mesh plane_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build(const Mesh& m, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(m, {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

bool same_positions(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

}  // namespace

TEST_CASE("a hierarchy round-trips, detail and active levels included") {
    Mesh base = plane_quads(4, 2.0f);
    base.uvs.resize(base.positions.size());
    for (std::size_t v = 0; v < base.positions.size(); ++v)
        base.uvs[v] = cf2(base.positions[v].x * 0.25f + 0.5f, base.positions[v].z * 0.25f + 0.5f);
    base.normals.assign(base.positions.size(), cf3(0, 1, 0));

    MultiresSurface s = build(base, 3);
    // Detail on two levels, not all of them: an empty level must round-trip as
    // an empty level rather than as a level of zeros.
    for (std::uint32_t v = 0; v < s.topology_at(2).vertex_count; v += 5)
        s.set_detail(2, v, LocalDetail{0.001f * static_cast<float>(v), 0.0f, 0.02f});
    for (std::uint32_t v = 0; v < s.topology_at(3).vertex_count; v += 17)
        s.set_detail(3, v, LocalDetail{0.0f, 0.0f, -0.004f});
    REQUIRE(s.set_sculpt_level(2));
    REQUIRE(s.set_display_level(3));

    const std::vector<cfloat3> fine = s.positions_at(3);
    const std::uint64_t sum = s.detail_checksum();

    const std::vector<std::uint8_t> bytes = s.encode();
    // Deterministic: two encodings of the same surface are the same bytes.
    CHECK(s.encode() == bytes);

    MultiresSurface back;
    REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.valid());
    CHECK(back.rule() == s.rule());
    CHECK(back.level_count() == s.level_count());
    CHECK(back.sculpt_level() == 2);
    CHECK(back.display_level() == 3);
    CHECK(back.detail_checksum() == sum);
    CHECK(back.detail_at(1).empty());
    CHECK_FALSE(back.detail_at(2).empty());

    // The cage comes back exactly, attributes included.
    REQUIRE(back.base_mesh().positions.size() == base.positions.size());
    CHECK(same_positions(back.base_mesh().positions, base.positions));
    CHECK(back.base_mesh().indices == base.indices);
    CHECK(back.base_mesh().quads == base.quads);
    REQUIRE(back.base_mesh().uvs.size() == base.uvs.size());
    for (std::size_t v = 0; v < base.uvs.size(); ++v) {
        CHECK(back.base_mesh().uvs[v].x == base.uvs[v].x);
        CHECK(back.base_mesh().uvs[v].y == base.uvs[v].y);
    }

    // And so does the surface it reconstructs — bit for bit, which is what
    // makes the face lists safe to leave out of the stream.
    CHECK(same_positions(back.positions_at(3), fine));
    CHECK(back.encode() == bytes);
}

TEST_CASE("a hierarchy with no detail round-trips to one with no detail") {
    MultiresSurface s = build(plane_quads(3, 1.0f), 2);
    const std::vector<std::uint8_t> bytes = s.encode();
    MultiresSurface back;
    REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.level_count() == 3);
    for (std::uint32_t l = 0; l < back.level_count(); ++l) CHECK(back.detail_at(l).empty());
    CHECK(same_positions(back.positions_at(2), s.positions_at(2)));
}

TEST_CASE("a triangle cage round-trips as a triangle cage") {
    Mesh tetra;
    tetra.positions = {cf3(1, 1, 1), cf3(1, -1, -1), cf3(-1, 1, -1), cf3(-1, -1, 1)};
    tetra.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    MultiresSurface s = build(tetra, 2);
    const std::vector<std::uint8_t> bytes = s.encode();
    MultiresSurface back;
    REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.base_mesh().quads.empty());
    CHECK(back.base_mesh().indices == tetra.indices);
    CHECK(same_positions(back.positions_at(2), s.positions_at(2)));
}

TEST_CASE("the decoder refuses a truncated, hostile or unknown buffer") {
    MultiresSurface s = build(plane_quads(3, 1.0f), 2);
    s.set_detail(2, 3, LocalDetail{0.0f, 0.0f, 0.1f});
    const std::vector<std::uint8_t> bytes = s.encode();

    MultiresSurface out;
    CHECK_FALSE(MultiresSurface::decode(nullptr, 0, &out));
    CHECK_FALSE(MultiresSurface::decode(bytes.data(), 0, &out));
    CHECK_FALSE(MultiresSurface::decode(bytes.data(), 8, &out));
    for (std::size_t cut = 12; cut < bytes.size(); cut += 37)
        CHECK_FALSE(MultiresSurface::decode(bytes.data(), cut, &out));

    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;
    CHECK_FALSE(MultiresSurface::decode(wrong.data(), wrong.size(), &out));

    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 42;
    CHECK_FALSE(MultiresSurface::decode(newer.data(), newer.size(), &out));

    // A rule this build does not implement. Reconstructing with the wrong one
    // would produce a different surface and nothing else in the stream would
    // say so, which is exactly why the rule is recorded.
    std::vector<std::uint8_t> other_rule = bytes;
    other_rule[8] = 7;
    CHECK_FALSE(MultiresSurface::decode(other_rule.data(), other_rule.size(), &out));

    // An active level past the end of the hierarchy it names.
    std::vector<std::uint8_t> bad_level = bytes;
    bad_level[16] = 9;
    CHECK_FALSE(MultiresSurface::decode(bad_level.data(), bad_level.size(), &out));

    // A POSITION COUNT larger than the buffer could hold — how a reader gets
    // asked to allocate gigabytes out of a few hundred bytes.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[28] = 0xff;
    hostile[29] = 0xff;
    hostile[30] = 0xff;
    hostile[31] = 0x0f;
    CHECK_FALSE(MultiresSurface::decode(hostile.data(), hostile.size(), &out));
}

TEST_CASE("a hostile depth is refused before anything is allocated") {
    // The stream is TINY and the hierarchy it declares is not: a cage of a few
    // thousand quads at twelve levels is billions of vertices. The counts
    // follow from the cage by arithmetic, so the refusal costs one loop rather
    // than eleven levels of allocation.
    MultiresSurface s = build(plane_quads(24, 2.0f), 1);
    std::vector<std::uint8_t> bytes = s.encode();
    CHECK(bytes.size() < 64 * 1024);

    // Level count 12 — the ceiling itself — over that cage.
    bytes[12] = 12;
    bytes[13] = 0;
    bytes[14] = 0;
    bytes[15] = 0;
    MultiresSurface out;
    CHECK_FALSE(MultiresSurface::decode(bytes.data(), bytes.size(), &out));

    // And a level count past the ceiling is refused on sight.
    bytes[12] = 200;
    CHECK_FALSE(MultiresSurface::decode(bytes.data(), bytes.size(), &out));
}

TEST_CASE("a level's detail must describe that level") {
    // A stream pairing one level's coefficients with a different vertex count
    // would silently attach every wrinkle somewhere else, so the counts are
    // checked rather than trusted.
    MultiresSurface s = build(plane_quads(3, 1.0f), 2);
    s.set_detail(1, 2, LocalDetail{0.0f, 0.0f, 0.1f});
    std::vector<std::uint8_t> bytes = s.encode();

    // Find the first detail blob by its own magic ('CMDF') and rewrite the
    // vertex count it declares.
    std::size_t at = bytes.size();
    for (std::size_t i = 0; i + 12 <= bytes.size(); ++i)
        if (bytes[i] == 0x43 && bytes[i + 1] == 0x4D && bytes[i + 2] == 0x44 &&
            bytes[i + 3] == 0x46) {
            at = i;
            break;
        }
    REQUIRE(at < bytes.size());
    bytes[at + 8] = 3;  // a vertex count no level of this hierarchy has
    bytes[at + 9] = 0;
    bytes[at + 10] = 0;
    bytes[at + 11] = 0;

    MultiresSurface out;
    CHECK_FALSE(MultiresSurface::decode(bytes.data(), bytes.size(), &out));
}
