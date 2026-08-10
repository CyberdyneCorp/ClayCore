// Mesh layers (scene-model + file-io specs): a document CARRIES triangles.
// The two properties everything else rests on are that the geometry survives
// the round trip element for element, and that its presence changes nothing
// about what the document evaluates to.

#include <doctest/doctest.h>

#include <cstring>

#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;

namespace {

// Every attribute populated, with values that are exact in binary32 and values
// that are not, so a lossy codec shows up rather than rounding into agreement.
mesh::Mesh attributed_mesh() {
    mesh::Mesh m;
    for (int i = 0; i < 12; ++i) {
        float f = static_cast<float>(i);
        m.positions.push_back(cf3(f * 0.1f, -f * 0.3333333f, 1e-8f * f));
        m.normals.push_back(cf3(0.5773503f, -0.5773503f, 0.5773503f));
        m.colors.push_back(cf3(f / 11.0f, 0.25f, 1.0f));
        m.uvs.push_back(cf2(f / 7.0f, 1.0f - f / 13.0f));
    }
    for (std::uint32_t t = 0; t + 2 < 12; ++t) {
        m.indices.push_back(t);
        m.indices.push_back(t + 1);
        m.indices.push_back(t + 2);
    }
    return m;
}

mesh::Mesh bare_mesh() {
    mesh::Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0)};
    m.indices = {0, 1, 2};
    return m;
}

scene::LayerId add_mesh_layer(io::ClaySpaceDoc* cs, const char* name, mesh::Mesh m) {
    scene::Layer& l = cs->document.add_sdf_layer(name);
    l.kind = scene::LayerKind::Mesh;
    l.sdf.reset();
    cs->mesh_layers.emplace(l.id, std::move(m));
    return l.id;
}

bool same_mesh(const mesh::Mesh& a, const mesh::Mesh& b) {
    if (a.positions.size() != b.positions.size() || a.normals.size() != b.normals.size() ||
        a.colors.size() != b.colors.size() || a.uvs.size() != b.uvs.size() ||
        a.indices != b.indices)
        return false;
    auto f3_equal = [](const std::vector<cfloat3>& x, const std::vector<cfloat3>& y) {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i].x != y[i].x || x[i].y != y[i].y || x[i].z != y[i].z) return false;
        return true;
    };
    if (!f3_equal(a.positions, b.positions) || !f3_equal(a.normals, b.normals) ||
        !f3_equal(a.colors, b.colors))
        return false;
    for (std::size_t i = 0; i < a.uvs.size(); ++i)
        if (a.uvs[i].x != b.uvs[i].x || a.uvs[i].y != b.uvs[i].y) return false;
    return true;
}

constexpr std::uint32_t kMeshFourcc = 'M' | ('E' << 8) | ('S' << 16) | ('H' << 24);

// Walk the container's chunks, handing each to `visit` as (fourcc, payload
// offset, payload length). The tests below rewrite and strip mesh chunks, and
// doing that by hand in each of them would be its own source of bugs.
template <typename Visit>
void each_chunk(const std::vector<std::uint8_t>& bytes, Visit visit) {
    std::size_t at = 8;  // magic + major + minor
    while (at + 12 <= bytes.size()) {
        std::uint32_t cc = 0;
        std::uint64_t len = 0;
        std::memcpy(&cc, bytes.data() + at, 4);
        std::memcpy(&len, bytes.data() + at + 4, 8);
        std::size_t payload = at + 12;
        if (payload + len > bytes.size()) return;
        visit(cc, payload, static_cast<std::size_t>(len));
        at = payload + static_cast<std::size_t>(len);
    }
}

// What a reader written before mesh layers existed sees: the same file with
// every MESH chunk unknown and therefore skipped.
std::vector<std::uint8_t> without_mesh_chunks(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + 8);
    each_chunk(bytes, [&](std::uint32_t cc, std::size_t payload, std::size_t len) {
        if (cc == kMeshFourcc) return;
        out.insert(out.end(), bytes.begin() + (payload - 12), bytes.begin() + payload + len);
    });
    return out;
}

// The first MESH chunk's mesh stream, as an offset into the container.
std::size_t mesh_stream_offset(const std::vector<std::uint8_t>& bytes) {
    std::size_t found = 0;
    each_chunk(bytes, [&](std::uint32_t cc, std::size_t payload, std::size_t) {
        if (cc == kMeshFourcc && !found) found = payload + 4;  // past the u32 layer id
    });
    return found;
}

}  // namespace

TEST_CASE("mesh layers: bit-identical round trip, attributes element for element") {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    const mesh::Mesh full = attributed_mesh();
    const mesh::Mesh bare = bare_mesh();
    scene::LayerId a = add_mesh_layer(&cs, "scan", full);
    scene::LayerId b = add_mesh_layer(&cs, "reference", bare);
    cs.document.find_layer(a)->xform.position = cf3(0.3f, -0.2f, 1.5f);

    std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    CHECK(io::save_clayspace(back) == bytes);  // canonical bit-identity

    REQUIRE(back.mesh_layers.size() == 2);
    CHECK(same_mesh(back.mesh_layers.at(a), full));
    CHECK(same_mesh(back.mesh_layers.at(b), bare));
    // A mesh with no attributes stays that way rather than being filled in.
    CHECK(back.mesh_layers.at(b).normals.empty());
    CHECK(back.mesh_layers.at(b).colors.empty());
    CHECK(back.mesh_layers.at(b).uvs.empty());
    // The layer keeps its own transform; the stored vertices do not move.
    CHECK(back.document.find_layer(a)->xform.position.z == 1.5f);
    CHECK(back.document.find_layer(a)->kind == scene::LayerKind::Mesh);
}

TEST_CASE("mesh layers: evaluation is unchanged by their presence") {
    io::ClaySpaceDoc plain;
    plain.document = gnarly_document();
    io::ClaySpaceDoc with_mesh;
    with_mesh.document = gnarly_document();
    add_mesh_layer(&with_mesh, "scan", attributed_mesh());

    scene::Tape a = scene::compile_document(plain.document);
    scene::Tape b = scene::compile_document(with_mesh.document);
    CHECK(a.instrs.size() == b.instrs.size());
    CHECK(a.params.size() == b.params.size());
    clay_test::Lcg rng(4242);
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(a.eval(p).d == b.eval(p).d);  // exact
    }
    // Not an operand, and not instanceable — the refusal instance_layer
    // already makes for a voxel layer.
    scene::LayerId id = with_mesh.document.layers.back().id;
    CHECK(with_mesh.document.instance_layer(id, "copy") == nullptr);
}

TEST_CASE("mesh layers: a reader that predates them skips the chunks") {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    scene::Layer& vl = cs.document.add_sdf_layer("blocks");
    vl.kind = scene::LayerKind::Voxel;
    vl.sdf.reset();
    voxel::VoxelGrid grid(0.1f);
    grid.fill_box({0, 0, 0}, {3, 3, 3}, grid.palette_add(cf3(1, 0.5f, 0)));
    cs.voxel_layers.emplace(vl.id, std::move(grid));
    add_mesh_layer(&cs, "scan", attributed_mesh());

    std::vector<std::uint8_t> stripped = without_mesh_chunks(io::save_clayspace(cs));
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(stripped.data(), stripped.size(), &back).ok());
    CHECK(back.mesh_layers.empty());
    CHECK(back.voxel_layers.size() == 1);
    CHECK(back.document.layers.size() == cs.document.layers.size());
    // Nothing else was misread as something it is not.
    scene::Tape a = scene::compile_document(cs.document);
    scene::Tape b = scene::compile_document(back.document);
    clay_test::Lcg rng(77);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(a.eval(p).d == b.eval(p).d);
    }
}

TEST_CASE("mesh layers: the major does not move, so nothing is refused") {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    add_mesh_layer(&cs, "scan", bare_mesh());
    std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
    CHECK(bytes[4] == io::kClaySpaceMajor);
    CHECK(bytes[6] == 5);  // the minor moved instead
    io::ClaySpaceDoc back;
    CHECK(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
}

TEST_CASE("mesh layers: a chunk and its layer stay matched") {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    scene::LayerId id = add_mesh_layer(&cs, "scan", attributed_mesh());

    SUBCASE("an orphaned payload is not written") {
        cs.document.remove_layer(id);
        CHECK(cs.mesh_layers.count(id) == 1);  // never erased on removal
        std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
        bool wrote_mesh = false;
        each_chunk(bytes, [&](std::uint32_t cc, std::size_t, std::size_t) {
            if (cc == kMeshFourcc) wrote_mesh = true;
        });
        CHECK_FALSE(wrote_mesh);
        io::ClaySpaceDoc back;
        REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
        CHECK(back.mesh_layers.empty());
    }

    SUBCASE("an unmatched chunk is dropped rather than refused") {
        std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
        // Repoint the chunk at a layer id the document does not carry.
        std::size_t at = 0;
        each_chunk(bytes, [&](std::uint32_t cc, std::size_t payload, std::size_t) {
            if (cc == kMeshFourcc && !at) at = payload;
        });
        REQUIRE(at != 0);
        std::uint32_t stranger = 9999;
        std::memcpy(bytes.data() + at, &stranger, 4);
        io::ClaySpaceDoc back;
        REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
        CHECK(back.mesh_layers.empty());
    }
}

TEST_CASE("mesh stream: malformed chunks are refused before anything is allocated") {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    add_mesh_layer(&cs, "scan", attributed_mesh());
    const std::vector<std::uint8_t> good = io::save_clayspace(cs);
    const std::size_t stream = mesh_stream_offset(good);
    REQUIRE(stream != 0);
    io::ClaySpaceDoc out;

    auto poke_u32 = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::uint8_t> bytes = good;
        std::memcpy(bytes.data() + offset, &value, 4);
        return io::load_clayspace(bytes.data(), bytes.size(), &out);
    };

    // over-declared vertex count: more vertices than the remaining bytes hold
    CHECK(poke_u32(stream, 4000000u).error == io::IoError::Malformed);
    // an index count that is not a whole number of triangles
    CHECK(poke_u32(stream + 4, 31u).error == io::IoError::Malformed);
    // an index at or past the vertex count — the bound that matters, because a
    // host reads these buffers by borrowed pointer
    std::vector<std::uint8_t> bad_index = good;
    std::size_t indices_at = bad_index.size() - 4 * 30;  // 30 indices, last chunk written
    std::uint32_t past_the_end = 12;
    std::memcpy(bad_index.data() + indices_at, &past_the_end, 4);
    CHECK(io::load_clayspace(bad_index.data(), bad_index.size(), &out).error ==
          io::IoError::Malformed);
    // an unknown attribute bit changes the stride, so the declared size stops
    // matching what is there
    std::vector<std::uint8_t> bad_mask = good;
    bad_mask[stream + 8] = 0xFF;
    CHECK(io::load_clayspace(bad_mask.data(), bad_mask.size(), &out).error ==
          io::IoError::Malformed);

    CHECK(io::load_clayspace(good.data(), good.size(), &out).ok());  // still loads
}

TEST_CASE("mesh stream: truncation at every prefix fails cleanly") {
    std::vector<std::uint8_t> bytes = io::save_mesh_stream(attributed_mesh());
    mesh::Mesh out;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        CHECK_FALSE(io::load_mesh_stream(bytes.data(), cut, &out).ok());
    CHECK(io::load_mesh_stream(bytes.data(), bytes.size(), &out).ok());
    CHECK(same_mesh(out, attributed_mesh()));
}
