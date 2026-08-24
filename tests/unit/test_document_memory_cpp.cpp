#include <doctest/doctest.h>

#include "clay/io/memory.h"
#include "clay/session/history.h"
#include "clay/voxel/mask.h"

// The half of the transient figure the C ABI cannot reach.
//
// An embedder driving session::History directly CAN hold a mask step open
// across several edits — which is the case the field exists for, and the only
// place it is ever non-zero. See test_document_memory.cpp for the ABI half,
// which asserts the opposite for the opposite reason.

using namespace clay;

TEST_CASE("memory: a held-open mask step is transient, and released") {
    io::ClaySpaceDoc doc;
    scene::Layer& l = doc.document.add_sdf_layer("body");
    doc.masks.emplace(l.id, voxel::MaskField(0.05f));
    voxel::MaskField& mask = doc.masks.at(l.id);

    session::History history;
    history.set_enabled(true);

    // Paint something first, so the snapshot has content to copy: the snapshot
    // is LAZY and taken on the first touch inside the step, so opening a step
    // on an empty mask would copy nothing and this would assert on zeros.
    mask.fill({{-0.3f, -0.3f, -0.3f}, {0.3f, 0.3f, 0.3f}}, 1.0f);
    REQUIRE(mask.painted_count() > 1000);

    const io::MemoryReport before = io::document_memory(doc, &history);
    REQUIRE(before.transient == 0);
    REQUIRE(before.masks > 0);

    REQUIRE(history.begin_mask_step(l.id, mask));
    // Still zero: arming a step costs nothing until something is touched, which
    // is the property that makes a step on an unedited mask free.
    CHECK(io::document_memory(doc, &history).transient == 0);

    mask.fill({{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}}, 0.5f);
    const io::MemoryReport during = io::document_memory(doc, &history);
    CHECK(during.transient > 0);
    // Roughly a doubling: the snapshot is a copy of the painted chunks.
    CHECK(during.transient >= before.masks / 2);
    CHECK(during.total > before.total);

    history.end_mask_step(mask);
    const io::MemoryReport after = io::document_memory(doc, &history);
    CHECK(after.transient == 0);
    // The mask itself is still there — only the copy went.
    CHECK(after.masks > 0);
}
