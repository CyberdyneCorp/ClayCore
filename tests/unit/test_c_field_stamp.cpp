// Placing one captured field many times (scene-model spec, stamp-a-captured-field).
//
// WHAT THIS FILE IS FOR, and it is not what the implementation guide assumed.
// The guide asks for an asset table so that "a thousand uses of one 4 MB asset
// must not consume ~4 GB", and `scene/types.h` says the sharing is already
// there: "A sampled volume. Held by shared reference on the Node, so instancing
// one costs a pointer rather than a copy of its samples." Both are true and
// they do not meet.
//
// The sharing is a `std::shared_ptr<const field::FieldVolume>` on the Node, and
// it survives exactly as long as the document stays in memory. The writer
// serializes a node's volume INSIDE the node record — `src/scene/commands.cpp`,
// `volume_bytes = n.volume->serialize(...)` per node — and the reader rebuilds
// one per node with `std::make_shared<field::FieldVolume>(...)`. So N placements
// of one capture write N copies and load as N unrelated volumes.
//
// That is the gap, and it is worth a test rather than a paragraph: the in-memory
// claim is true, so anyone checking it in a debugger sees sharing and stops
// looking.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

struct CItem {
    clay_item* item = nullptr;
    ~CItem() { clay_item_destroy(item); }
    CItem() = default;
    CItem(const CItem&) = delete;
    CItem& operator=(const CItem&) = delete;
};

clay_layer_id sphere_layer(clay_document* doc) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "src", &layer) == CLAY_OK);
    clay_item_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = 1.0f;
    d.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
    return layer;
}

// A capture of a finite world region of the document's field, which is what
// `clay_item_volume_from_document` already is.
void capture(const clay_document* doc, float cell, clay_item** out) {
    clay_volume_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.cell_size = cell;
    const float lo[3] = {-1.25f, -1.25f, -1.25f};
    const float hi[3] = {1.25f, 1.25f, 1.25f};
    REQUIRE(clay_item_volume_from_document(doc, &p, lo, hi, out) == CLAY_OK);
    REQUIRE(*out != nullptr);
}

std::size_t saved_bytes(const clay_document* doc) {
    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc, &blob) == CLAY_OK);
    const std::size_t size = clay_blob_size(blob);
    clay_blob_destroy(blob);
    return size;
}

// One document holding `placements` copies of the same captured volume.
std::size_t document_with(int placements, float cell) {
    CDoc src;
    sphere_layer(src.doc);
    CItem stamp;
    capture(src.doc, cell, &stamp.item);

    CDoc target;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(target.doc, "stamps", &layer) == CLAY_OK);
    for (int i = 0; i < placements; ++i) {
        // The SAME item, moved and placed again. Position lives on the node;
        // the samples stay behind the one shared_ptr the item holds, so this is
        // eight placements of one payload in memory.
        const float at[3] = {static_cast<float>(i) * 3.0f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(stamp.item, at) == CLAY_OK);
        clay_node_id node = 0;
        REQUIRE(clay_layer_add_item(target.doc, layer, stamp.item, &node) == CLAY_OK);
    }
    return saved_bytes(target.doc);
}

}  // namespace

TEST_CASE("field stamp: placing one capture many times shares its payload on disk") {
    // The claim the guide makes and this feature has to hold: a thousand uses of
    // one asset must not cost a thousand payloads. Measured as BYTES rather than
    // as a pointer comparison, because the pointer comparison passes today and
    // the bytes do not — the sharing is in memory only, and a document is a
    // thing you save.
    const float kCell = 0.08f;
    const std::size_t one = document_with(1, kCell);
    const std::size_t eight = document_with(8, kCell);

    MESSAGE("saved bytes: 1 placement " << one << ", 8 placements " << eight);
    REQUIRE(one > 0);

    // Eight placements of one asset should cost one payload plus eight small
    // references, NOT eight payloads. A generous ceiling: anything under twice
    // the single-placement document is unambiguously shared, and eight copies
    // would be about eight times it.
    CHECK(eight < one * 2);
}

TEST_CASE("field stamp: a reloaded document still shares one payload") {
    // The half that is easy to lose. Even a writer that deduplicates has to be
    // met by a reader that rebuilds ONE volume and points every placement at
    // it; a reader that calls make_shared per node loads N unrelated copies and
    // the next save writes N payloads again.
    const float kCell = 0.08f;
    CDoc src;
    sphere_layer(src.doc);
    CItem stamp;
    capture(src.doc, kCell, &stamp.item);

    CDoc target;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(target.doc, "stamps", &layer) == CLAY_OK);
    for (int i = 0; i < 8; ++i) {
        const float at[3] = {static_cast<float>(i) * 3.0f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(stamp.item, at) == CLAY_OK);
        clay_node_id node = 0;
        REQUIRE(clay_layer_add_item(target.doc, layer, stamp.item, &node) == CLAY_OK);
    }

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(target.doc, &blob) == CLAY_OK);
    const std::size_t first = clay_blob_size(blob);

    clay_document* reloaded = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &reloaded) ==
            CLAY_OK);
    clay_blob_destroy(blob);
    REQUIRE(reloaded != nullptr);

    const std::size_t again = saved_bytes(reloaded);
    clay_document_destroy(reloaded);

    MESSAGE("saved " << first << ", reloaded and saved again " << again);
    // A round trip must not grow the document. If the reader split one shared
    // payload into eight, the second save writes eight of them.
    CHECK(again <= first);
}
