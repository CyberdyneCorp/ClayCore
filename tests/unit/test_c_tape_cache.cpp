#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "clay.h"

// The compiled tape is cached on the document and invalidated by a revision the
// mutating entry points bump. Under-bumping is the dangerous direction: the
// document changes, the cache does not, and every later read answers with the
// field as it was — silently, with no error and no crash.
//
// So this suite walks the mutating surface of the ABI and, for each entry point,
// requires that what the field ANSWERS afterwards has changed. A test that only
// checked the call returned CLAY_OK would pass against a permanently stale
// cache, which is exactly the bug being guarded.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_sphere(Doc& doc, float r, float x) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

// A fingerprint of what the document currently evaluates to: distance and
// colour over a spread of probes. Any change the tape should have picked up
// moves at least one of these.
std::vector<float> field_signature(const clay_document* d) {
    const int kProbes = 40;
    std::vector<float> pts(kProbes * 3);
    for (int i = 0; i < kProbes; ++i) {
        pts[i * 3 + 0] = -1.2f + 0.06f * static_cast<float>(i);
        pts[i * 3 + 1] = 0.11f * static_cast<float>(i % 7) - 0.3f;
        pts[i * 3 + 2] = 0.09f * static_cast<float>(i % 5) - 0.2f;
    }
    std::vector<float> dist(kProbes), colour(kProbes * 3);
    REQUIRE(clay_eval_points(d, nullptr, pts.data(), kProbes, dist.data(), colour.data()) ==
            CLAY_OK);
    std::vector<float> sig = dist;
    sig.insert(sig.end(), colour.begin(), colour.end());
    float scale = 0.0f;
    REQUIRE(clay_safe_step_scale(d, &scale) == CLAY_OK);
    sig.push_back(scale);
    return sig;
}

// What picking answers, which uses a different tape (ghosted layers excluded)
// and so has its own cache slot.
std::vector<float> pick_signature(const clay_document* d) {
    const int kRays = 12;
    std::vector<float> rays(kRays * 6);
    for (int i = 0; i < kRays; ++i) {
        rays[i * 6 + 0] = -0.5f + 0.1f * static_cast<float>(i);
        rays[i * 6 + 1] = 0.0f;
        rays[i * 6 + 2] = 5.0f;
        rays[i * 6 + 3] = 0.0f;
        rays[i * 6 + 4] = 0.0f;
        rays[i * 6 + 5] = -1.0f;
    }
    std::vector<std::int32_t> hit(kRays);
    std::vector<float> t(kRays), pos(kRays * 3), nrm(kRays * 3);
    REQUIRE(clay_raycast_many(d, rays.data(), kRays, hit.data(), t.data(), pos.data(),
                              nrm.data()) == CLAY_OK);
    std::vector<float> sig;
    for (int i = 0; i < kRays; ++i) {
        sig.push_back(static_cast<float>(hit[i]));
        sig.push_back(hit[i] ? t[i] : 0.0f);
    }
    return sig;
}

}  // namespace

TEST_CASE("tape cache: every mutating entry point is seen by the next read") {
    // Each block reads the field, mutates through one entry point, and reads
    // again. `before != after` is the assertion — a cache that failed to
    // invalidate returns the identical signature.
    SUBCASE("adding an item") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        add_sphere(doc, 0.4f, 0.9f);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("adding an item through the flat descriptor") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        clay_item_desc desc;
        std::memset(&desc, 0, sizeof desc);
        desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
        desc.prim = CLAY_PRIM_BOX;
        desc.params[0] = desc.params[1] = desc.params[2] = 0.3f;
        desc.position[0] = 0.8f;
        desc.rotation[3] = 1.0f;
        desc.scale = 1.0f;
        REQUIRE(clay_add_item(doc.d, doc.layer, &desc, nullptr) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("removing a node") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        clay_node_id second = add_sphere(doc, 0.4f, 0.9f);
        std::vector<float> before = field_signature(doc.d);
        REQUIRE(clay_remove_node(doc.d, doc.layer, second) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("moving a node's transform") {
        Doc doc;
        clay_node_id n = add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float pos[3] = {0.7f, 0.0f, 0.0f};
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        REQUIRE(clay_layer_set_transform(doc.d, doc.layer, n, pos, axis, 0.0f, 1.0f) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("replacing a node's primitive") {
        Doc doc;
        clay_node_id n = add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float box[3] = {0.4f, 0.2f, 0.6f};
        REQUIRE(clay_layer_set_prim(doc.d, doc.layer, n, CLAY_PRIM_BOX, box, 3) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("recolouring a node") {
        // Colour does not move the surface, so this one is caught only because
        // the signature carries the colour field as well as the distance.
        Doc doc;
        clay_node_id n = add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float rgb[3] = {0.1f, 0.8f, 0.3f};
        REQUIRE(clay_layer_set_color(doc.d, doc.layer, n, rgb) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("changing op and blend") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        clay_node_id n = add_sphere(doc, 0.4f, 0.6f);
        std::vector<float> before = field_signature(doc.d);
        REQUIRE(clay_layer_set_op_blend(doc.d, doc.layer, n, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                        0.3f, 0.0f) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("adding a deformer") {
        Doc doc;
        clay_node_id n = add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float twist[1] = {1.5f};
        REQUIRE(clay_layer_add_deformer(doc.d, doc.layer, n, CLAY_DEFORM_TWIST, twist, 1, CLAY_EASE_LINEAR, 0) ==
                CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("appending to and trimming a stroke") {
        Doc doc;
        const float xyzr[8] = {-0.3f, 0.0f, 0.0f, 0.2f, 0.3f, 0.0f, 0.0f, 0.2f};
        clay_item* it = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
        REQUIRE(it != nullptr);
        REQUIRE(clay_item_set_stroke_points(it, xyzr, 2) == CLAY_OK);
        clay_node_id n = 0;
        REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &n) == CLAY_OK);
        clay_item_destroy(it);

        std::vector<float> before = field_signature(doc.d);
        const float more[4] = {0.9f, 0.4f, 0.0f, 0.25f};
        REQUIRE(clay_layer_append_stroke(doc.d, doc.layer, n, more, 1) == CLAY_OK);
        std::vector<float> appended = field_signature(doc.d);
        CHECK(appended != before);

        REQUIRE(clay_layer_trim_stroke(doc.d, doc.layer, n, 1) == CLAY_OK);
        CHECK(field_signature(doc.d) != appended);
    }

    SUBCASE("hiding a layer") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.layer, 0) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("transforming a layer") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float pos[3] = {0.6f, 0.1f, 0.0f};
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        REQUIRE(clay_document_set_layer_transform(doc.d, doc.layer, pos, axis, 0.3f, 1.0f) ==
                CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("mirroring a layer") {
        // The layer's mirror axes apply only to items that opted in, so the
        // item carries the flag: a layer mirror over un-mirrored items is
        // correctly a no-op and would not test anything.
        Doc doc;
        const float r = 0.4f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float pos[3] = {0.6f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_item_set_mirror(it, 1) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);

        std::vector<float> before = field_signature(doc.d);
        REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("removing a layer") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        clay_layer_id second = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "other", &second) == CLAY_OK);
        const float r = 0.4f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float pos[3] = {0.9f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc.d, second, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);

        std::vector<float> before = field_signature(doc.d);
        REQUIRE(clay_document_remove_layer(doc.d, second) == CLAY_OK);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("adding a layer that then receives geometry") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        clay_layer_id second = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "other", &second) == CLAY_OK);
        const float r = 0.4f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float pos[3] = {0.95f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc.d, second, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("applying a brush stroke") {
        Doc doc;
        add_sphere(doc, 0.6f, 0.0f);
        std::vector<float> before = field_signature(doc.d);
        const float br = 0.1f;
        clay_item* brush = clay_item_create(CLAY_PRIM_SPHERE, &br, 1);
        REQUIRE(brush != nullptr);
        clay_stroke_preset preset;
        std::memset(&preset, 0, sizeof preset);
        preset.struct_size = static_cast<std::uint32_t>(sizeof preset);
        preset.radius = 0.1f;
        preset.spacing = 0.5f;
        preset.strength = 1.0f;
        const float samples[10] = {0.55f, 0.0f, 0.0f, 1.0f, 0.0f,
                                   0.55f, 0.3f, 0.0f, 1.0f, 1.0f};
        std::size_t written = 0;
        REQUIRE(clay_layer_apply_stroke(doc.d, doc.layer, samples, 2, &preset, brush, nullptr,
                                        nullptr, &written) == CLAY_OK);
        clay_item_destroy(brush);
        REQUIRE(written > 0);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("undo and redo") {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
        std::vector<float> before = field_signature(doc.d);
        add_sphere(doc, 0.4f, 0.9f);
        std::vector<float> added = field_signature(doc.d);
        REQUIRE(added != before);

        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
        REQUIRE(undone == 1);
        CHECK(field_signature(doc.d) == before);  // back to exactly the old field

        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(doc.d, &redone) == CLAY_OK);
        REQUIRE(redone == 1);
        CHECK(field_signature(doc.d) == added);
    }
}

TEST_CASE("tape cache: the picking tape has its own invalidation") {
    // Ghosting changes what picking sees and nothing about what the field
    // evaluates to, so the two tapes are different and cached separately. A
    // shared slot would answer one of them from the other.
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f);
    clay_layer_id front = 0;
    REQUIRE(clay_add_sdf_layer(doc.d, "front", &front) == CLAY_OK);
    const float r = 0.3f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    const float pos[3] = {0.0f, 0.0f, 1.2f};  // nearer the camera than the body
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc.d, front, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);

    std::vector<float> pick_before = pick_signature(doc.d);
    std::vector<float> field_before = field_signature(doc.d);

    // Ghost the near layer: picking must stop seeing it...
    REQUIRE(clay_document_set_layer_protection(doc.d, front, 1, 0) == CLAY_OK);
    CHECK(pick_signature(doc.d) != pick_before);
    // ...while the field it evaluates to is unchanged, ghost being a picking
    // and authoring flag rather than a visibility one.
    CHECK(field_signature(doc.d) == field_before);

    // Un-ghosting restores exactly the original picking answer.
    REQUIRE(clay_document_set_layer_protection(doc.d, front, 0, 0) == CLAY_OK);
    CHECK(pick_signature(doc.d) == pick_before);
}

TEST_CASE("tape cache: repeated reads of an unchanged document agree") {
    // The cheap half of the contract: a cache hit must return what a compile
    // would have returned, not merely something.
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f);
    add_sphere(doc, 0.35f, 0.7f);
    std::vector<float> first = field_signature(doc.d);
    for (int i = 0; i < 8; ++i) CHECK(field_signature(doc.d) == first);
    std::vector<float> pick_first = pick_signature(doc.d);
    for (int i = 0; i < 8; ++i) CHECK(pick_signature(doc.d) == pick_first);
}

TEST_CASE("tape cache: a loaded document reads as itself, not as its source") {
    // Save/load builds a second document; each carries its own cache, and a
    // fresh one must not answer from an empty or borrowed slot.
    const std::string path = "clay_tape_cache_probe.clayspace";
    std::vector<float> saved_signature;
    {
        Doc doc;
        add_sphere(doc, 0.5f, 0.0f);
        add_sphere(doc, 0.3f, 0.8f);
        saved_signature = field_signature(doc.d);
        REQUIRE(clay_document_save(doc.d, path.c_str()) == CLAY_OK);
    }
    clay_document* loaded = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &loaded) == CLAY_OK);
    REQUIRE(loaded != nullptr);
    CHECK(field_signature(loaded) == saved_signature);
    // ...and it invalidates like any other document
    std::vector<float> before = field_signature(loaded);
    clay_layer_id first_layer = 0;
    const float r = 0.25f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    const float pos[3] = {-0.9f, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    // layer ids start at 1 for a document written by this library
    first_layer = 1;
    REQUIRE(clay_layer_add_item(loaded, first_layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
    CHECK(field_signature(loaded) != before);
    clay_document_destroy(loaded);
    std::remove(path.c_str());
}

TEST_CASE("tape cache: concurrent readers of one document agree") {
    // Before the cache, two threads could read one document at once:
    // compile_document takes a const Document& and returned a fresh tape, so
    // there was no shared mutable state. The cache introduces some, and must
    // not take that property away — hence the snapshot handed to each reader
    // rather than a reference into a slot another thread may rebuild.
    Doc doc;
    for (int i = 0; i < 40; ++i) add_sphere(doc, 0.1f + 0.01f * static_cast<float>(i), 0.05f * i);

    const float probe[3] = {0.3f, 0.1f, 0.0f};
    float reference = 0.0f;
    REQUIRE(clay_eval_points(doc.d, nullptr, probe, 1, &reference, nullptr) == CLAY_OK);

    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int k = 0; k < 500; ++k) {
                float d = 0.0f;
                if (clay_eval_points(doc.d, nullptr, probe, 1, &d, nullptr) != CLAY_OK ||
                    d != reference)
                    mismatches.fetch_add(1);
                const float o[3] = {0.0f, 0.0f, 5.0f}, dir[3] = {0.0f, 0.0f, -1.0f};
                std::int32_t hit = 0;
                float ht = 0.0f, hp[3], hn[3];
                clay_raycast(doc.d, o, dir, &hit, &ht, hp, hn);
            }
        });
    }
    for (std::thread& w : workers) w.join();
    CHECK(mismatches.load() == 0);
}
