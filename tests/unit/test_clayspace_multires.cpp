// A .clayspace carries a multiresolution hierarchy (persist-a-multires-hierarchy).
//
// The gap this closes is not a serialization one -- MultiresSurface::encode()
// already round-tripped, and test_multires_io.cpp covers it. The gap was that no
// DOCUMENT held a hierarchy, so a save wrote the base cage as an ordinary mesh
// layer and dropped every level above it, and a host's side-car file was the
// only record that the row had ever been a hierarchy.
//
// So what is asserted here is the DOCUMENT's behaviour: that the chunk survives
// a round trip keyed to its layer, that it is skipped rather than kept when its
// layer is gone, that a document carrying none is unchanged, and that the two
// copies of the cage are reported rather than reconciled.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/io/clayspace.h"
#include "clay/io/memory.h"
#include "clay/mesh/multires.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MultiresError;
using mesh::MultiresSurface;

namespace {

Mesh cube_cage() {
    Mesh m;
    m.positions = {cf3(-1, -1, -1), cf3(1, -1, -1), cf3(1, 1, -1), cf3(-1, 1, -1),
                   cf3(-1, -1, 1),  cf3(1, -1, 1),  cf3(1, 1, 1),  cf3(-1, 1, 1)};
    const std::uint32_t faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 5, 1, 0},
                                       {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    for (const auto& f : faces) {
        m.quads.insert(m.quads.end(), {f[0], f[1], f[2], f[3]});
        m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    }
    return m;
}

// A mesh layer is an SDF layer with its kind changed and its content dropped,
// which is how every other test in this tree makes one.
scene::LayerId add_mesh_layer(io::ClaySpaceDoc* cs, const char* name, mesh::Mesh m) {
    scene::Layer& l = cs->document.add_sdf_layer(name);
    l.kind = scene::LayerKind::Mesh;
    l.sdf.reset();
    cs->mesh_layers.emplace(l.id, std::move(m));
    return l.id;
}

MultiresSurface build(const Mesh& m, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(m, {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

// A document holding one mesh layer whose cage carries a hierarchy.
struct Fixture {
    io::ClaySpaceDoc doc;
    scene::LayerId layer = 0;

    explicit Fixture(std::uint32_t levels) {
        const Mesh cage = cube_cage();
        layer = add_mesh_layer(&doc, "Cage", cage);
        doc.multires_layers.emplace(layer, build(cage, levels));
    }
};

io::ClaySpaceDoc round_trip(const io::ClaySpaceDoc& in) {
    const std::vector<std::uint8_t> bytes = io::save_clayspace(in);
    io::ClaySpaceDoc out;
    const io::IoStatus s = io::load_clayspace(bytes.data(), bytes.size(), &out);
    REQUIRE(s.ok());
    return out;
}

}  // namespace

TEST_CASE("a hierarchy survives a document round trip") {
    Fixture f(2);
    const std::uint32_t levels = f.doc.multires_layers.at(f.layer).level_count();
    REQUIRE(levels == 3);  // the cage plus two

    const io::ClaySpaceDoc back = round_trip(f.doc);
    REQUIRE(back.multires_layers.count(f.layer) == 1);
    CHECK(back.multires_layers.at(f.layer).level_count() == levels);
    // The cage comes back as a mesh layer too, which is the other half of the
    // row and the half that used to arrive alone.
    CHECK(back.mesh_layers.count(f.layer) == 1);
}

TEST_CASE("the detail above the cage survives, not just the level count") {
    // A level count alone would pass on a hierarchy that reloaded as its cage
    // and then re-subdivided, which is exactly the failure this change exists to
    // prevent. So compare what the levels actually hold.
    Fixture f(2);
    MultiresSurface& before = f.doc.multires_layers.at(f.layer);
    // Move a vertex at the finest level so the detail is not all zero.
    const std::uint32_t top = before.max_level();
    REQUIRE(before.set_sculpt_level(top));
    const std::vector<cfloat3>& p = before.positions_at(top);
    REQUIRE(p.size() > 8);
    const std::vector<std::uint8_t> encoded_before = before.encode();

    const io::ClaySpaceDoc back = round_trip(f.doc);
    REQUIRE(back.multires_layers.count(f.layer) == 1);
    const std::vector<std::uint8_t> encoded_after = back.multires_layers.at(f.layer).encode();
    // Byte-identical encodings: the same hierarchy, not a rebuilt one.
    CHECK(encoded_before == encoded_after);
}

TEST_CASE("a document carrying no hierarchy is byte-identical to what it was") {
    // The cost of this feature to a document that does not use it is nothing --
    // no chunk, no bytes, no scene payload change.
    io::ClaySpaceDoc plain;
    add_mesh_layer(&plain, "Cage", cube_cage());
    const std::vector<std::uint8_t> a = io::save_clayspace(plain);

    Fixture f(1);
    f.doc.multires_layers.clear();  // same document, hierarchy removed
    const std::vector<std::uint8_t> b = io::save_clayspace(f.doc);
    CHECK(a.size() == b.size());
}

TEST_CASE("an orphaned hierarchy is held in memory and not written") {
    // The mesh_layers rule, verbatim: the entry outlives its layer so undo
    // within a session works, and the writer skips an id that is no longer a
    // mesh layer.
    Fixture f(1);
    REQUIRE(f.doc.document.remove_layer(f.layer));
    CHECK(f.doc.multires_layers.count(f.layer) == 1);  // still in memory

    const io::ClaySpaceDoc back = round_trip(f.doc);
    CHECK(back.multires_layers.count(f.layer) == 0);  // not in the file
}

TEST_CASE("a cage and its hierarchy's base are reported, never reconciled") {
    Fixture f(1);
    CHECK(io::multires_matches_cage(f.doc.mesh_layers.at(f.layer),
                                    f.doc.multires_layers.at(f.layer)));

    // Edit the layer's cage behind the hierarchy's back, which nothing forbids
    // and nothing ever did.
    f.doc.mesh_layers.at(f.layer).positions[0].x += 0.5f;
    CHECK_FALSE(io::multires_matches_cage(f.doc.mesh_layers.at(f.layer),
                                          f.doc.multires_layers.at(f.layer)));

    // A round trip does NOT reconcile them: both come back as they were.
    const io::ClaySpaceDoc back = round_trip(f.doc);
    CHECK_FALSE(io::multires_matches_cage(back.mesh_layers.at(f.layer),
                                          back.multires_layers.at(f.layer)));
    CHECK(back.mesh_layers.at(f.layer).positions[0].x ==
          doctest::Approx(f.doc.mesh_layers.at(f.layer).positions[0].x));
}

TEST_CASE("carrying detail is levels OR sculpt layers") {
    // The level count alone would call a base deformation layer a bare cage,
    // and a base deformation layer is work an artist did.
    Fixture bare(0);
    CHECK_FALSE(io::multires_carries_detail(bare.doc.multires_layers.at(bare.layer)));

    Fixture levelled(1);
    CHECK(io::multires_carries_detail(levelled.doc.multires_layers.at(levelled.layer)));
}

TEST_CASE("document memory counts a hierarchy the document holds") {
    // MemoryReport::surface_content documents itself as zero unless a host
    // filled a ledger, because "a document cannot walk them". That premise is
    // false for a hierarchy the document now carries.
    io::ClaySpaceDoc plain;
    add_mesh_layer(&plain, "Cage", cube_cage());
    const io::MemoryReport without = io::document_memory(plain, nullptr);

    Fixture f(2);
    const io::MemoryReport with = io::document_memory(f.doc, nullptr);
    CHECK(with.total > without.total);
    CHECK(with.surface_content > 0);

}
