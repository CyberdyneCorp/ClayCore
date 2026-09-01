// WHAT THE STACK'S BYTE FORM REFUSES, and what it composes to whatever route
// it took to get there (file-io and mesh-sculpt-layers specs,
// add-mesh-sculpt-layers).
//
// The sibling file `test_mesh_sculpt_layer_history.cpp` already gates the
// gesture and property records — magic, version, op code, truncation, an absurd
// declared count. This one gates the two things it does not reach:
//
//   * THE STACK CHUNK ITSELF, which is the only part of this format a hostile
//     document actually reaches. It rides inside the multires stream, and
//     `MultiresSurface::decode` checks a decoded stack against the hierarchy it
//     built — AFTER `SculptLayerStack::decode` has already reserved from every
//     number the chunk declares. Everything the chunk claims about itself has
//     to be refused here or not at all;
//   * COMPOSITION IS ROUTE-INDEPENDENT. A stack that arrived through a save and
//     a load, and a stack the same operations built in memory, must evaluate to
//     the same bits — and so must a surface composed block by block as the
//     operations landed and one composed once at the end. A block cache that
//     misses an invalidation produces a surface that is right the first time
//     and wrong ever after, and only the warm-against-cold comparison sees it.
//
// THE STREAMS ARE BUILT RATHER THAN POKED. `StackStream` below writes the same
// layout `SculptLayerStack::encode` writes, using the REAL field encoders for
// the parts with structure, and the first case asserts that a well-formed
// `StackStream` is byte-identical to what the engine produces. That assertion
// is what keeps every hostile case honest: if the format moves, this file fails
// on the round trip rather than quietly poking a neighbouring field and passing
// for a reason nobody intended.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_layer.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DetailField;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MultiresSurface;
using mesh::SculptLayerId;
using mesh::SculptLayerKind;
using mesh::SculptLayerStack;
using mesh::SparseWeightField;

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
            const std::uint32_t a = static_cast<std::uint32_t>(z) * stride +
                                    static_cast<std::uint32_t>(x);
            m.quads.insert(m.quads.end(), {a, a + 1, a + stride + 1, a + stride});
            m.indices.insert(m.indices.end(),
                             {a, a + 1, a + stride + 1, a, a + stride + 1, a + stride});
        }
    return m;
}

MultiresSurface build(int levels) {
    auto surface = MultiresSurface::from_mesh(plane_quads(4, 1.0f));
    REQUIRE(surface.has_value());
    for (int i = 0; i < levels; ++i) REQUIRE(surface->add_level());
    return std::move(*surface);
}

LocalDetail lift(float n) {
    LocalDetail d;
    d.normal = n;
    return d;
}

bool bit_equal(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// -- a stack stream, written from parts ---------------------------------------

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
}
void put_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
    put_u32(out, static_cast<std::uint32_t>(v & 0xffffffffu));
    put_u32(out, static_cast<std::uint32_t>(v >> 32));
}
void put_f32(std::vector<std::uint8_t>* out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}
void put_blob(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& blob) {
    put_u32(out, static_cast<std::uint32_t>(blob.size()));
    out->insert(out->end(), blob.begin(), blob.end());
}

// One layer's payload, exactly as `encode_layer` writes it. `kind` and
// `name_size` are settable independently of the name, which is the whole reason
// this exists rather than a call to the engine's own encoder.
struct LayerStream {
    std::uint64_t id = 1;
    std::uint32_t kind = static_cast<std::uint32_t>(SculptLayerKind::Sampled);
    std::uint32_t flags = 1;  // visible, unlocked
    float strength = 1.0f;
    std::string name = "pass";
    // Declared independently of `name` so a stream can claim a length it does
    // not carry.
    std::uint32_t declared_name_size = 0xffffffffu;  // sentinel: use name.size()
    std::vector<DetailField> detail;
    std::vector<SparseWeightField> mask;
    std::uint32_t declared_levels = 0xffffffffu;  // sentinel: use detail.size()

    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        put_u64(&out, id);
        put_u32(&out, kind);
        put_u32(&out, flags);
        put_f32(&out, strength);
        put_u32(&out, declared_name_size == 0xffffffffu
                          ? static_cast<std::uint32_t>(name.size())
                          : declared_name_size);
        out.insert(out.end(), name.begin(), name.end());
        put_u32(&out, declared_levels == 0xffffffffu
                          ? static_cast<std::uint32_t>(detail.size())
                          : declared_levels);
        for (std::size_t l = 0; l < detail.size(); ++l) {
            put_blob(&out, detail[l].encode());
            put_blob(&out, mask[l].encode());
        }
        return out;
    }
};

struct StackStream {
    std::uint32_t magic = 0x534c4d43u;  // 'CMLS'
    std::uint32_t version = 1;
    std::uint32_t declared_count = 0xffffffffu;  // sentinel: use layers.size()
    std::uint64_t active = 0;
    std::uint64_t next_id = 2;
    std::uint32_t block_size = DetailField::kDefaultBlockSize;
    std::vector<std::uint32_t> level_vertices;
    std::vector<LayerStream> layers;

    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        put_u32(&out, magic);
        put_u32(&out, version);
        put_u32(&out, declared_count == 0xffffffffu
                          ? static_cast<std::uint32_t>(layers.size())
                          : declared_count);
        put_u64(&out, active);
        put_u64(&out, next_id);
        put_u32(&out, block_size);
        put_u32(&out, static_cast<std::uint32_t>(level_vertices.size()));
        for (std::uint32_t v : level_vertices) put_u32(&out, v);
        for (const LayerStream& l : layers) put_blob(&out, l.bytes());
        return out;
    }

    bool decodes() const {
        const std::vector<std::uint8_t> b = bytes();
        SculptLayerStack out;
        return SculptLayerStack::decode(b.data(), b.size(), &out);
    }
};

// A stream that describes the stack `surface` is carrying — the shape every
// hostile case starts from and changes exactly one thing about.
StackStream well_formed(const MultiresSurface& surface) {
    const SculptLayerStack& stack = surface.sculpt_layers();
    StackStream s;
    s.active = stack.active();
    s.block_size = stack.block_size();
    for (std::uint32_t l = 0; l < stack.level_count(); ++l)
        s.level_vertices.push_back(stack.level_vertex_count(l));
    std::uint64_t highest = 0;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const mesh::SculptLayer* layer = stack.at(i);
        LayerStream out;
        out.id = layer->id;
        out.kind = static_cast<std::uint32_t>(layer->kind);
        out.flags = (layer->visible ? 1u : 0u) | (layer->locked ? 2u : 0u);
        out.strength = layer->strength;
        out.name = layer->name;
        out.detail = layer->detail;
        out.mask = layer->mask;
        s.layers.push_back(std::move(out));
        highest = std::max<std::uint64_t>(highest, layer->id);
    }
    s.next_id = highest + 1;
    return s;
}

// The same stream with the layers taken out — and with the ACTIVE id taken out
// with them, because an active layer the stream does not carry is refused (a
// rule this file's own positive controls tripped over before they earned their
// keep).
StackStream layerless(const MultiresSurface& surface) {
    StackStream s = well_formed(surface);
    s.layers.clear();
    s.active = mesh::kNoSculptLayer;
    return s;
}

// A surface with two layers over three levels, which is the fixture every case
// below reads a stream out of.
MultiresSurface layered() {
    MultiresSurface surface = build(2);
    const SculptLayerId first = surface.add_sculpt_layer("wrinkles");
    const SculptLayerId second = surface.add_sculpt_layer("pores");
    surface.set_sculpt_layer_detail(first, 2, 40, lift(0.05f));
    surface.set_sculpt_layer_detail(second, 2, 41, lift(-0.02f));
    REQUIRE(surface.set_sculpt_layer_mask(second, 2, 41, 0.25f));
    REQUIRE(surface.set_sculpt_layer_strength(first, 0.5f));
    return surface;
}

}  // namespace

TEST_CASE("the hand-written stream is the engine's own, which is what makes the rest mean anything") {
    MultiresSurface surface = layered();
    // BYTE FOR BYTE. Every refusal below changes one field of `StackStream` and
    // claims the change is what the decoder objected to; that claim is only
    // worth something while the rest of the stream is indistinguishable from
    // what `encode` writes.
    CHECK(well_formed(surface).bytes() == surface.sculpt_layers().encode());
    CHECK(well_formed(surface).decodes());
}

TEST_CASE("a level count and a level size are ceilings, not whatever a chunk declares") {
    // THE ONE A DOCUMENT ACTUALLY REACHES. `MultiresSurface::decode` bounds its
    // own level count at `kMaxLevels` and each level at `kMaxLevelVertices`
    // before it builds anything, then checks the stack it decoded against the
    // hierarchy — but that check runs after this decoder has returned, and both
    // numbers are ones this decoder RESERVES FROM. A forty-eight-byte chunk
    // declaring three levels of four billion vertices was accepted, and
    // reserved three gigabytes of dirty index on the way through.
    MultiresSurface surface = layered();

    SUBCASE("more levels than this build reconstructs") {
        StackStream s = layerless(surface);
        s.level_vertices.assign(SculptLayerStack::kMaxLevels + 1, 4u);
        CHECK_FALSE(s.decodes());
        s.level_vertices.assign(SculptLayerStack::kMaxLevels, 4u);
        CHECK(s.decodes());
    }
    SUBCASE("a level larger than any level can be") {
        StackStream s = layerless(surface);
        s.level_vertices = {4u, SculptLayerStack::kMaxLevelVertices + 1u};
        CHECK_FALSE(s.decodes());
        s.level_vertices = {4u, SculptLayerStack::kMaxLevelVertices};
        CHECK(s.decodes());
    }
    SUBCASE("and the largest legal chunk is still a hundred bytes of work") {
        // The refusals above close the number a hostile chunk can name. What
        // they cannot close is the LARGEST LEGAL one — twelve levels of a
        // billion vertices at the finest blocking the format allows is a
        // request this decoder must accept, and used to reserve three gigabytes
        // of dirty index for. It does not any more, because `all` is the
        // resting state of a decoded stack and the mark array is unread while
        // it holds, so it is sized where it is first consulted instead.
        //
        // Accepted here, MEASURED in `test_sculpt_allocation.cpp`, which is the
        // one translation unit in this tree that can see an allocation at all.
        StackStream s = layerless(surface);
        s.block_size = 4;  // the finest blocking the format allows
        s.level_vertices.assign(SculptLayerStack::kMaxLevels,
                                SculptLayerStack::kMaxLevelVertices);
        const std::vector<std::uint8_t> b = s.bytes();
        REQUIRE(b.size() < 128);
        SculptLayerStack out;
        REQUIRE(SculptLayerStack::decode(b.data(), b.size(), &out));
        CHECK(out.size() == 0);
        CHECK(out.level_count() == SculptLayerStack::kMaxLevels);
    }
}

TEST_CASE("a layer's fields must describe this stack's levels and share its blocking") {
    MultiresSurface surface = layered();

    SUBCASE("a layer carrying a different number of levels") {
        StackStream s = well_formed(surface);
        s.layers[0].declared_levels = static_cast<std::uint32_t>(s.level_vertices.size()) - 1;
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("a coefficient field sized to another level") {
        StackStream s = well_formed(surface);
        // The same coefficients, described as a level with four vertices fewer.
        DetailField wrong;
        wrong.reset(s.level_vertices[2] - 4, s.block_size);
        wrong.set(40, lift(0.05f));
        s.layers[0].detail[2] = wrong;
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("a mask sized to another level") {
        StackStream s = well_formed(surface);
        SparseWeightField wrong;
        wrong.reset(s.level_vertices[2] - 4, s.block_size);
        wrong.set(41, 0.25f);
        s.layers[1].mask[2] = wrong;
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("a field blocked differently from the stack") {
        // NOT A MEMORY CLAIM — a correctness one, and the failure it produces
        // is silence. `note_layer_coverage` hands a FIELD's block numbers
        // straight to the STACK's dirty index without translating them, so a
        // coefficient at vertex 5000 is block 4 under the stack's 1024 and
        // block 1250 under a field's 4. Marking 1250 falls off the end of a
        // five-entry index, `note_block` drops it, and the strength slider
        // then invalidates nothing and leaves the surface composed from a
        // stack nobody dialled. A stream may not pair two blockings.
        StackStream s = well_formed(surface);
        REQUIRE(s.block_size != 4u);
        DetailField finer;
        finer.reset(s.level_vertices[2], 4u);
        finer.set(40, lift(0.05f));
        s.layers[0].detail[2] = finer;
        CHECK_FALSE(s.decodes());

        SparseWeightField finer_mask;
        finer_mask.reset(s.level_vertices[2], 4u);
        finer_mask.set(41, 0.25f);
        StackStream m = well_formed(surface);
        m.layers[1].mask[2] = finer_mask;
        CHECK_FALSE(m.decodes());
    }
    SUBCASE("an EMPTY field at a level the layer never reached is fine") {
        // The other half of the rule, which a decoder tightened past this point
        // would break: a layer made at level 2 stores nothing at levels 0 and
        // 1, and an empty field describes no level at all.
        StackStream s = well_formed(surface);
        REQUIRE(s.layers[0].detail[0].vertex_count() == 0);
        CHECK(s.decodes());
    }
}

TEST_CASE("the kind is read, and the one this build does not implement is refused") {
    // DECISION D3 / task 1.3, whose entire enforcement is one comparison in the
    // decoder. A reader that dropped a procedural pore layer it did not
    // understand would present a surface missing an artist's work while
    // claiming to be complete — the same failure the surface version bump
    // exists to prevent, one level down.
    MultiresSurface surface = layered();
    StackStream s = well_formed(surface);
    REQUIRE(s.layers[0].kind == static_cast<std::uint32_t>(SculptLayerKind::Sampled));

    s.layers[0].kind = static_cast<std::uint32_t>(SculptLayerKind::Procedural);
    CHECK_FALSE(s.decodes());
    s.layers[0].kind = 7;  // and a kind no version of this format has named
    CHECK_FALSE(s.decodes());
}

TEST_CASE("a strength a slider could not have produced is refused rather than trusted") {
    // Taking one on trust would let a file amplify a recorded pass past what
    // the artist made — a strength of 40 over a wrinkle pass is a different
    // sculpt, arriving through a document that claims to be this one.
    MultiresSurface surface = layered();
    for (float hostile : {-0.001f, 1.001f, 40.0f}) {
        StackStream s = well_formed(surface);
        s.layers[0].strength = hostile;
        CAPTURE(hostile);
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("and a strength that is not a number at all") {
        // `!(strength >= 0.0f)` rather than `strength < 0.0f`, because every
        // comparison against a NaN is false and the naive spelling admits it.
        StackStream s = well_formed(surface);
        s.layers[0].strength = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("the endpoints are not hostile") {
        for (float ok : {0.0f, 1.0f}) {
            StackStream s = well_formed(surface);
            s.layers[0].strength = ok;
            CAPTURE(ok);
            CHECK(s.decodes());
        }
    }
}

TEST_CASE("an id is a handle, so a stream may not make two of them the same one") {
    MultiresSurface surface = layered();

    SUBCASE("two layers answering to one id") {
        // Every lookup would be ambiguous, and every undo record naming that id
        // would apply to whichever came first.
        StackStream s = well_formed(surface);
        s.layers[1].id = s.layers[0].id;
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("the reserved id nothing may hold") {
        StackStream s = well_formed(surface);
        s.layers[0].id = mesh::kNoSculptLayer;
        CHECK_FALSE(s.decodes());
    }
    SUBCASE("an id at or above the counter that mints the next one") {
        // THE HAZARD THE COUNTER IS SERIALIZED FOR, arriving from the other
        // side: a stream whose counter has been rewound below an id it carries
        // would mint that id again on the next `add`, and the document would
        // hold two layers with one handle without a single malformed byte.
        StackStream s = well_formed(surface);
        s.next_id = s.layers[1].id;
        CHECK_FALSE(s.decodes());
        s.next_id = s.layers[1].id + 1;
        CHECK(s.decodes());
    }
    SUBCASE("and a decoded stack keeps minting above everything it carries") {
        const std::vector<std::uint8_t> b = well_formed(surface).bytes();
        SculptLayerStack out;
        REQUIRE(SculptLayerStack::decode(b.data(), b.size(), &out));
        const SculptLayerId minted = out.add("after the load");
        CHECK(minted != mesh::kNoSculptLayer);
        for (std::size_t i = 0; i + 1 < out.size(); ++i) CHECK(out.id_at(i) != minted);
    }
}

TEST_CASE("an active layer the stream does not carry is refused") {
    // `active` is an id rather than an index precisely so it survives a
    // reorder; the cost of that is that a stream can name one that is not
    // there, and a stack whose active layer does not exist would send the next
    // stroke to a channel nobody can see.
    MultiresSurface surface = layered();
    StackStream s = well_formed(surface);
    s.active = s.layers[1].id + 500;
    CHECK_FALSE(s.decodes());

    SUBCASE("no active layer at all is a state, not an error") {
        StackStream none = well_formed(surface);
        none.active = mesh::kNoSculptLayer;
        CHECK(none.decodes());
    }
}

TEST_CASE("a name is bounded, on the way in and on the way out") {
    MultiresSurface surface = layered();
    const std::string too_long(SculptLayerStack::kMaxNameBytes + 1, 'x');
    CHECK_FALSE(surface.rename_sculpt_layer(surface.sculpt_layers().id_at(0), too_long));
    CHECK(surface.rename_sculpt_layer(surface.sculpt_layers().id_at(0),
                                      std::string(SculptLayerStack::kMaxNameBytes, 'x')));

    SUBCASE("and a stream declaring more than the ceiling is refused") {
        // The name is at the ceiling, so the buffer genuinely carries enough
        // bytes for the declared length — which is what makes this the name
        // check rather than the truncation check firing early.
        StackStream s = well_formed(surface);
        REQUIRE(s.layers[0].name.size() == SculptLayerStack::kMaxNameBytes);
        s.layers[0].declared_name_size = SculptLayerStack::kMaxNameBytes + 1;
        CHECK_FALSE(s.decodes());
    }
}

TEST_CASE("the mask's own byte form round-trips and refuses the same six things") {
    // Task 2.7's mask is a container this change introduced, with its own
    // versioned form and its own decoder, and everything above reaches it only
    // through a layer. This is it on its own terms.
    SparseWeightField field;
    field.reset(4096, 1024);
    field.set(5, 0.25f);
    field.set(3000, 0.5f);
    field.set(4095, 0.0f);

    const std::vector<std::uint8_t> bytes = field.encode();
    SparseWeightField loaded;
    REQUIRE(SparseWeightField::decode(bytes.data(), bytes.size(), &loaded));
    CHECK(loaded.checksum() == field.checksum());
    CHECK(loaded.get(5) == doctest::Approx(0.25f));
    CHECK(loaded.get(3000) == doctest::Approx(0.5f));
    CHECK(loaded.get(4095) == doctest::Approx(0.0f));
    // THE IDENTITY IS ONE, and a round trip must not turn "untouched" into
    // "erased" — including on the tail of a stored block, which `encode`
    // writes as 1.0 for exactly this reason.
    CHECK(loaded.get(6) == doctest::Approx(1.0f));
    CHECK(loaded.get(2000) == doctest::Approx(1.0f));
    CHECK(loaded.stored_block_count() == field.stored_block_count());

    SUBCASE("a foreign magic and an unwritten version") {
        for (std::size_t at : {std::size_t{0}, std::size_t{4}}) {
            std::vector<std::uint8_t> hostile = bytes;
            hostile[at] ^= 0xffu;
            SparseWeightField out;
            CAPTURE(at);
            CHECK_FALSE(SparseWeightField::decode(hostile.data(), hostile.size(), &out));
        }
    }
    SUBCASE("a blocking this reader cannot address") {
        // Not a power of two, below the floor, and above the ceiling. A block
        // size taken on trust would put every offset below it out of step with
        // the stream.
        for (std::uint32_t hostile : {1000u, 2u, (1u << 20) + 1u}) {
            std::vector<std::uint8_t> b = bytes;
            std::memcpy(b.data() + 12, &hostile, 4);
            SparseWeightField out;
            CAPTURE(hostile);
            CHECK_FALSE(SparseWeightField::decode(b.data(), b.size(), &out));
        }
    }
    SUBCASE("a block index past the level it declares") {
        std::vector<std::uint8_t> b = bytes;
        const std::uint32_t past = 4096u / 1024u;  // one past the last block
        std::memcpy(b.data() + 20, &past, 4);
        SparseWeightField out;
        CHECK_FALSE(SparseWeightField::decode(b.data(), b.size(), &out));
    }
    SUBCASE("blocks that are not ascending describe several fields overlaid") {
        // `encode` writes them in order; a stream that repeats or reverses one
        // is describing two answers for the same vertices, and choosing one is
        // not a decoder's decision to make.
        REQUIRE(field.stored_block_count() == 3);
        const std::size_t per_block = 4 + 1024 * 4;
        std::vector<std::uint8_t> b = bytes;
        std::swap_ranges(b.begin() + 20, b.begin() + 20 + per_block,
                         b.begin() + 20 + per_block);
        SparseWeightField out;
        CHECK_FALSE(SparseWeightField::decode(b.data(), b.size(), &out));
    }
    SUBCASE("more blocks than the buffer could hold, refused before allocation") {
        // Four blocks fit in the level and only three are written, so this
        // passes the "past the level" test and has to be refused by the
        // arithmetic that prices the count against the bytes that follow it.
        std::vector<std::uint8_t> b = bytes;
        const std::uint32_t declared = 4;
        std::memcpy(b.data() + 16, &declared, 4);
        SparseWeightField out;
        CHECK_FALSE(SparseWeightField::decode(b.data(), b.size(), &out));
    }
    SUBCASE("a truncated stream at every plausible cut") {
        for (std::size_t cut : {std::size_t{0}, std::size_t{10}, bytes.size() / 2,
                                bytes.size() - 1}) {
            SparseWeightField out;
            CAPTURE(cut);
            CHECK_FALSE(SparseWeightField::decode(bytes.data(), cut, &out));
        }
    }
}

TEST_CASE("composition does not depend on how the stack got here") {
    // THE PROPERTY A BLOCK CACHE CAN LOSE WITHOUT ANYONE NOTICING. Each of
    // these routes must produce the same surface to the bit:
    //
    //   * WARM — the operations interleaved with evaluations, so every one of
    //     them landed against a populated composed cache and had to invalidate
    //     it correctly;
    //   * COLD — the same operations with a single evaluation at the end, so
    //     every block composes once from scratch;
    //   * RELOADED — the warm surface through a save and a load.
    //
    // A missed invalidation shows up as warm != cold and as nothing else: the
    // cold surface is right, the reloaded surface is right, and the one the
    // artist is looking at is stale.
    const auto run = [](MultiresSurface* s, bool evaluate_as_we_go) {
        const auto settle = [&] {
            if (evaluate_as_we_go) s->positions_at(2);
        };
        const SculptLayerId a = s->add_sculpt_layer("form");
        const SculptLayerId b = s->add_sculpt_layer("wrinkles");
        const SculptLayerId c = s->add_sculpt_layer("pores");
        for (std::uint32_t v = 30; v < 70; ++v) s->set_sculpt_layer_detail(a, 2, v, lift(0.03f));
        settle();
        for (std::uint32_t v = 50; v < 90; ++v) s->set_sculpt_layer_detail(b, 2, v, lift(-0.01f));
        settle();
        for (std::uint32_t v = 60; v < 65; ++v) s->set_sculpt_layer_detail(c, 2, v, lift(0.007f));
        settle();
        REQUIRE(s->set_sculpt_layer_strength(a, 0.37f));
        settle();
        REQUIRE(s->set_sculpt_layer_mask(b, 2, 55, 0.5f));
        settle();
        REQUIRE(s->move_sculpt_layer(c, 0));
        settle();
        REQUIRE(s->set_sculpt_layer_visible(b, false));
        settle();
        REQUIRE(s->set_sculpt_layer_visible(b, true));
        settle();
        REQUIRE(s->set_sculpt_layer_strength(a, 1.0f));
        settle();
        REQUIRE(s->remove_sculpt_layer(c));
        settle();
        return b;
    };

    MultiresSurface warm = build(2);
    warm.set_detail(2, 40, lift(0.05f));
    run(&warm, true);

    MultiresSurface cold = build(2);
    cold.set_detail(2, 40, lift(0.05f));
    run(&cold, false);

    CHECK(bit_equal(warm.positions_at(2), cold.positions_at(2)));
    CHECK(warm.sculpt_layer_checksum() == cold.sculpt_layer_checksum());
    // Every level, not only the one the strokes reached — a level nothing
    // touched must still agree, which is where a stale composed field that was
    // never invalidated would sit unnoticed.
    for (std::uint32_t l = 0; l <= 2; ++l) {
        CAPTURE(l);
        CHECK(bit_equal(warm.positions_at(l), cold.positions_at(l)));
    }

    SUBCASE("and a save and a load is a fourth route to the same bits") {
        const std::vector<std::uint8_t> bytes = warm.encode();
        MultiresSurface reloaded;
        REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &reloaded));
        CHECK(reloaded.sculpt_layer_checksum() == warm.sculpt_layer_checksum());
        for (std::uint32_t l = 0; l <= 2; ++l) {
            CAPTURE(l);
            CHECK(bit_equal(warm.positions_at(l), reloaded.positions_at(l)));
        }
        // And the reloaded stack still invalidates: a decoded stack whose dirty
        // index was never sized is exactly the shape that would compose once
        // and then go deaf, so the slider has to move the surface again.
        const std::vector<cfloat3> before = reloaded.positions_at(2);
        REQUIRE(reloaded.set_sculpt_layer_strength(reloaded.sculpt_layers().id_at(0), 0.0f));
        CHECK_FALSE(bit_equal(before, reloaded.positions_at(2)));
        REQUIRE(reloaded.set_sculpt_layer_strength(reloaded.sculpt_layers().id_at(0), 1.0f));
        CHECK(bit_equal(before, reloaded.positions_at(2)));
    }
}
