// Attribute transfer between meshes (add-mesh-attribute-transfer).
//
// The claim this rests on is the identity case: a mesh given its own
// attributes back must return them bit-identically, because that is the only
// case where an exact answer exists and therefore the only honest exactness
// test. It holds for every vertex whose position is UNIQUE — and the exception
// is the documented one, pinned below rather than left to be discovered.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "clay/mesh/marching.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/transfer.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf2;
using kernel::cf3;

namespace {

// A displaced grid: every vertex position distinct, no seam, no pole.
Mesh grid(int n = 12) {
    Mesh m;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float x = static_cast<float>(i) / n, z = static_cast<float>(j) / n;
            m.positions.push_back(cf3(x, 0.1f * std::sin(i * 0.7f) * std::cos(j * 0.5f), z));
            m.colors.push_back(cf3(x, z, 0.25f));
            m.uvs.push_back(cf2(x, z));
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j * (n + 1) + i);
            const std::uint32_t b = a + 1, c = a + static_cast<std::uint32_t>(n + 1), d = c + 1;
            for (std::uint32_t k : {a, c, b, b, c, d}) m.indices.push_back(k);
        }
    return m;
}

// A CLOSED sphere, for the round trip. `to_field` takes its sign from a
// generalized winding number, which is undefined on an open sheet — the grid
// above is a height field and sampling it gives a field with no inside. Colour
// is a smooth ramp in x so a transferred vertex can be checked against its own
// position.
Mesh sphere(int n = 20) {
    Mesh m;
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j < n; ++j) {
            const float a = 3.14159265f * static_cast<float>(i) / n;
            const float b = 6.28318531f * static_cast<float>(j) / n;
            const kernel::cfloat3 p =
                cf3(std::sin(a) * std::cos(b), std::cos(a), std::sin(a) * std::sin(b));
            m.positions.push_back(p);
            m.colors.push_back(cf3(p.x * 0.5f + 0.5f, 0.25f, 0.75f));
        }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const std::uint32_t a = static_cast<std::uint32_t>(i * n + j);
            const std::uint32_t b = static_cast<std::uint32_t>(i * n + (j + 1) % n);
            const std::uint32_t c = a + static_cast<std::uint32_t>(n);
            const std::uint32_t d = b + static_cast<std::uint32_t>(n);
            // Wound so the outward normal points out: to_field takes its sign
            // from a winding number, and the inverted order gives a field
            // that is positive everywhere — no inside, and nothing to mesh.
            for (std::uint32_t k : {a, b, c, b, d, c}) m.indices.push_back(k);
        }
    return m;
}

bool same(const kernel::cfloat3& a, const kernel::cfloat3& b) {
    return std::memcmp(&a, &b, sizeof(kernel::cfloat3)) == 0;
}
bool same(const kernel::cfloat2& a, const kernel::cfloat2& b) {
    return std::memcmp(&a, &b, sizeof(kernel::cfloat2)) == 0;
}

}  // namespace

TEST_CASE("mesh transfer: an identity transfer is bit-exact") {
    const Mesh src = grid();
    Mesh tgt = src;
    tgt.colors.clear();
    tgt.uvs.clear();

    const TransferReport r = transfer_attributes(src, &tgt);
    CHECK(r.transferred == src.positions.size());
    CHECK(r.fell_back == 0);
    CHECK(r.colors);
    CHECK(r.uvs);

    for (std::size_t i = 0; i < src.positions.size(); ++i) {
        REQUIRE(same(tgt.colors[i], src.colors[i]));
        REQUIRE(same(tgt.uvs[i], src.uvs[i]));
    }
}

TEST_CASE("mesh transfer: a corner query is exact even when the barycentrics are not") {
    // The regression for the only platform split this change had. `Bvh::closest`
    // returns an EXACT corner — w == 1, u == v == 0 — for a query landing on a
    // vertex on x86, and a hair off it on Apple silicon, where the dot products
    // behind it contract to fma. The weighted sum is bit-exact only in the first
    // case, so "an identity transfer is bit-exact" passed on one platform and
    // failed on the other. Feeding the off-corner barycentrics in directly
    // reproduces the macOS failure anywhere.
    const Mesh src = grid();

    const kernel::cfloat3 c0 = sample_color(src, 0, 1e-7f, 1e-7f, cf3(0, 0, 0));
    CHECK(same(c0, src.colors[src.indices[0]]));

    const kernel::cfloat3 c1 = sample_color(src, 0, 1.0f - 2e-7f, 1e-7f, cf3(0, 0, 0));
    CHECK(same(c1, src.colors[src.indices[1]]));

    const kernel::cfloat3 c2 = sample_color(src, 0, 1e-7f, 1.0f - 2e-7f, cf3(0, 0, 0));
    CHECK(same(c2, src.colors[src.indices[2]]));

    const kernel::cfloat2 t0 = sample_uv(src, 0, 1e-7f, 1e-7f, cf2(0, 0));
    CHECK(same(t0, src.uvs[src.indices[0]]));

    // Snapping is confined to the corners: a query anywhere else still blends,
    // so the fix cannot be hiding a mesh that returns nothing but vertex values.
    const kernel::cfloat3 mid = sample_color(src, 0, 0.5f, 0.25f, cf3(0, 0, 0));
    CHECK_FALSE(same(mid, src.colors[src.indices[0]]));
    CHECK_FALSE(same(mid, src.colors[src.indices[1]]));
    CHECK_FALSE(same(mid, src.colors[src.indices[2]]));
}

TEST_CASE("mesh transfer: nothing moves") {
    const Mesh src = grid();
    Mesh tgt = src;
    const std::vector<kernel::cfloat3> before_p = tgt.positions;
    const std::vector<std::uint32_t> before_i = tgt.indices;

    TransferOptions all;
    all.normals = true;
    transfer_attributes(src, &tgt, all);

    CHECK(std::memcmp(tgt.positions.data(), before_p.data(),
                      before_p.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(std::memcmp(tgt.indices.data(), before_i.data(),
                      before_i.size() * sizeof(std::uint32_t)) == 0);
}

TEST_CASE("mesh transfer: normals are off by default") {
    // A resampled mesh should shade like ITSELF. Taking the source's normals
    // would make new geometry shade like the old shape, so the default is off
    // and this pins it rather than trusting the header.
    Mesh src = grid();
    src.normals.assign(src.positions.size(), cf3(0, 1, 0));
    Mesh tgt = grid();
    tgt.normals.assign(tgt.positions.size(), cf3(1, 0, 0));

    const TransferReport off = transfer_attributes(src, &tgt);
    CHECK_FALSE(off.normals);
    CHECK(same(tgt.normals[0], cf3(1, 0, 0)));  // untouched

    TransferOptions on;
    on.normals = true;
    const TransferReport got = transfer_attributes(src, &tgt, on);
    CHECK(got.normals);
    CHECK(same(tgt.normals[0], cf3(0, 1, 0)));
}

TEST_CASE("mesh transfer: geometry the source never occupied falls back, and says so") {
    const Mesh src = grid();
    Mesh tgt = grid();
    // Move half the target far away — after a boolean, geometry exists where
    // the source never was, and the closest point to it means nothing.
    for (std::size_t i = 0; i < tgt.positions.size(); i += 2)
        tgt.positions[i] = tgt.positions[i] + cf3(0, 50.0f, 0);

    const TransferReport r = transfer_attributes(src, &tgt);
    CHECK(r.fell_back > 0);
    CHECK(r.transferred > 0);
    CHECK(r.transferred + r.fell_back == tgt.positions.size());
    // The threshold was derived from the source's own size, not guessed.
    CHECK(r.max_distance > 0.0f);

    SUBCASE("an explicit threshold overrides the derived one") {
        Mesh far = grid();
        for (kernel::cfloat3& p : far.positions) p = p + cf3(0, 50.0f, 0);
        TransferOptions generous;
        generous.max_distance = 100.0f;
        const TransferReport all = transfer_attributes(src, &far, generous);
        CHECK(all.fell_back == 0);
        CHECK(all.max_distance == 100.0f);
    }
}

TEST_CASE("mesh transfer: a channel the source lacks is left alone, not cleared") {
    // So that a caller can take colour from one mesh and uvs from another. The
    // report is what makes "nothing happened" visible.
    Mesh src = grid();
    src.uvs.clear();  // source has colours but no uvs
    Mesh tgt = grid();
    const std::vector<kernel::cfloat2> before = tgt.uvs;

    const TransferReport r = transfer_attributes(src, &tgt);
    CHECK(r.colors);
    CHECK_FALSE(r.uvs);
    REQUIRE(tgt.uvs.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) REQUIRE(same(tgt.uvs[i], before[i]));
}

TEST_CASE("mesh transfer: deterministic") {
    const Mesh src = grid();
    Mesh a = grid(), b = grid();
    a.colors.clear();
    b.colors.clear();
    transfer_attributes(src, &a);
    transfer_attributes(src, &b);
    REQUIRE(a.colors.size() == b.colors.size());
    for (std::size_t i = 0; i < a.colors.size(); ++i) REQUIRE(same(a.colors[i], b.colors[i]));
}

TEST_CASE("mesh transfer: the uv seam limitation, pinned rather than discovered") {
    // Uvs are per VERTEX, which is HOW a seam exists: the source duplicates a
    // position into two vertices carrying different uvs. A target vertex on
    // that seam has one slot and two correct answers, and takes whichever
    // triangle the closest-point query returned.
    //
    // This test DOCUMENTS that. It is not a failure to be fixed later — it
    // follows from per-vertex uvs — and colour is unaffected because colour is
    // continuous across a seam.
    Mesh src = grid(4);
    // Duplicate vertex 0 with a different uv, as a seam does.
    src.positions.push_back(src.positions[0]);
    src.colors.push_back(src.colors[0]);
    src.uvs.push_back(cf2(0.99f, 0.99f));  // the other side of the layout
    const std::uint32_t twin = static_cast<std::uint32_t>(src.positions.size() - 1);
    // Give the twin a triangle of its own so the query can land on it.
    src.indices.push_back(twin);
    src.indices.push_back(src.indices[1]);
    src.indices.push_back(src.indices[2]);

    Mesh tgt;
    tgt.positions.push_back(src.positions[0]);  // exactly on the seam
    tgt.indices = {0, 0, 0};

    const TransferReport r = transfer_attributes(src, &tgt);
    REQUIRE(r.transferred == 1);
    // ONE of the two right answers — which one is not specified, and asserting
    // a particular one would be asserting an implementation detail.
    const bool took_one_side = same(tgt.uvs[0], src.uvs[0]) || same(tgt.uvs[0], cf2(0.99f, 0.99f));
    CHECK(took_one_side);
    // Colour is the same from either side, so it has no such ambiguity.
    CHECK(same(tgt.colors[0], src.colors[0]));
}

TEST_CASE("mesh transfer: colour survives a trip through the field") {
    // The case this exists for. A coloured mesh sampled into a field and
    // meshed back is new geometry with no colours; the transfer refunds them.
    //
    // What is measured is HOW closely, not that it happened: the target's
    // vertices are somewhere else entirely, so an exact answer does not exist
    // and a test asserting one would be asserting the resample.
    const Mesh src = sphere();
    ImportSettings settings;
    settings.cell_size = 0.06f;
    const std::optional<field::FieldVolume> vol = to_field(src, settings);
    REQUIRE(vol.has_value());

    const math::Aabb box = vol->bounds();
    const float spacing = 0.06f;
    const kernel::cfloat3 origin = box.min - cf3(spacing, spacing, spacing);
    const auto extent = [&](float lo, float hi) {
        return static_cast<int>(std::ceil((hi - lo) / spacing)) + 3;
    };
    int cell_min[3] = {0, 0, 0};
    int cell_max[3] = {extent(box.min.x, box.max.x), extent(box.min.y, box.max.y),
                       extent(box.min.z, box.max.z)};
    Mesh back = mesh_lattice(
        [&](int i, int j, int k) {
            return vol->eval(origin + cf3(static_cast<float>(i), static_cast<float>(j),
                                          static_cast<float>(k)) *
                                          spacing);
        },
        cell_min, cell_max, origin, spacing);
    REQUIRE(back.positions.size() > 100);
    CHECK(back.colors.empty());  // the field carried none

    const TransferReport r = transfer_attributes(src, &back);
    CHECK(r.colors);
    CHECK(r.transferred > back.positions.size() / 2);

    // The source's colour is a smooth ramp in x, so a transferred vertex should
    // carry roughly the ramp value at its own position.
    double worst = 0.0;
    std::size_t checked = 0;
    for (std::size_t i = 0; i < back.positions.size(); ++i) {
        const kernel::cfloat3 p = back.positions[i];
        worst = std::max(worst,
                         static_cast<double>(std::fabs(back.colors[i].x - (p.x * 0.5f + 0.5f))));
        ++checked;
    }
    CHECK(checked > 100);
    // Within a cell's worth of the ramp. Not exact, and the number is the
    // point: this refunds the paint, not the geometry.
    CHECK(worst < 0.10);
    (void)box;
}
