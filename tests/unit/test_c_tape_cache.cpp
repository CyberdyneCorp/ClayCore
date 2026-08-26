#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
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

// Appending an item is the one edit the cache rebuilds by REUSING the tape it
// already had, compiling only the appended node onto the compiled prefix
// rather than re-emitting the document. That fast path is invisible when it
// works and silent when it is wrong: the read succeeds, nothing errors, and
// the answer is the field as it was some dabs ago.
//
// So these check what the fast path can get wrong, not that it was taken.
TEST_CASE("tape cache: an append is rebuilt by reuse and is still seen") {
    SUBCASE("each append in a stroke reads as a document built from scratch") {
        // The oracle is a FRESH document holding the same items: its first
        // read has no prefix to reuse and so is always a full compile. A
        // stale cache, or a prefix reused where it should not have been,
        // shows up as a disagreement with it.
        //
        // `before != after` would be the weaker test and a trap here: a dab
        // that lands entirely inside the base sphere changes no distance at
        // all, because min() is unchanged, so it would fail against perfectly
        // correct code.
        auto built_with = [](int dabs) {
            Doc fresh;
            add_sphere(fresh, 1.0f, 0.0f);
            for (int i = 1; i <= dabs; ++i)
                add_sphere(fresh, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
            return field_signature(fresh.d);
        };

        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        std::vector<float> base = field_signature(doc.d);
        // A stroke, not one dab: every append after the first resumes from
        // the tape the previous one produced, which is the path a single-dab
        // test never reaches.
        for (int i = 1; i <= 8; ++i) {
            add_sphere(doc, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
            CHECK(field_signature(doc.d) == built_with(i));
        }
        // And the stroke did move the field, so the agreement above is not
        // two identical readings of an unchanged document.
        add_sphere(doc, 0.9f, 0.9f);
        CHECK(field_signature(doc.d) != base);
    }

    SUBCASE("appending without reading in between still shows every item") {
        // The log accumulates across several edits and is consumed by one
        // rebuild; a rebuild that dropped all but the last would pass a
        // read-after-every-dab test and fail this one.
        Doc a;
        add_sphere(a, 1.0f, 0.0f);
        for (int i = 1; i <= 5; ++i) add_sphere(a, 0.25f, -0.9f + 0.2f * static_cast<float>(i));

        Doc b;
        add_sphere(b, 1.0f, 0.0f);
        for (int i = 1; i <= 5; ++i) {
            add_sphere(b, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
            field_signature(b.d);  // forces a rebuild per dab
        }
        CHECK(field_signature(a.d) == field_signature(b.d));
    }

    SUBCASE("an append reads the same as a document built without the cache") {
        // The fast path's answer against the answer of a document that never
        // had a prefix to reuse: same items, same order, so same field.
        Doc grown;
        add_sphere(grown, 1.0f, 0.0f);
        field_signature(grown.d);  // compile, so the append below resumes
        add_sphere(grown, 0.4f, 0.7f);

        Doc built;
        add_sphere(built, 1.0f, 0.0f);
        add_sphere(built, 0.4f, 0.7f);
        CHECK(field_signature(grown.d) == field_signature(built.d));
    }

    SUBCASE("undo after an append restores the field exactly") {
        Doc doc;
        REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
        add_sphere(doc, 1.0f, 0.0f);
        std::vector<float> before = field_signature(doc.d);

        add_sphere(doc, 0.4f, 0.7f);
        CHECK(field_signature(doc.d) != before);

        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
        REQUIRE(undone != 0);
        // Undo does not go through the append funnel, so it must fall back to
        // a full recompile. If the append log survived it, this reads as the
        // appended document and the sculpt silently refuses to undo.
        CHECK(field_signature(doc.d) == before);

        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(doc.d, &redone) == CLAY_OK);
        REQUIRE(redone != 0);
        CHECK(field_signature(doc.d) != before);
    }

    SUBCASE("an append interleaved with an ordinary edit") {
        // A parameter edit between two appends breaks the log's contiguity.
        // Reusing across it would answer with the pre-edit item.
        Doc doc;
        clay_node_id base = add_sphere(doc, 1.0f, 0.0f);
        add_sphere(doc, 0.4f, 0.7f);
        field_signature(doc.d);

        const float r = 0.55f;
        REQUIRE(clay_layer_set_prim(doc.d, doc.layer, base, CLAY_PRIM_SPHERE, &r, 1) == CLAY_OK);
        add_sphere(doc, 0.3f, -0.7f);

        Doc built;
        clay_node_id b0 = add_sphere(built, 1.0f, 0.0f);
        add_sphere(built, 0.4f, 0.7f);
        REQUIRE(clay_layer_set_prim(built.d, built.layer, b0, CLAY_PRIM_SPHERE, &r, 1) == CLAY_OK);
        add_sphere(built, 0.3f, -0.7f);
        CHECK(field_signature(doc.d) == field_signature(built.d));
    }

    SUBCASE("appending to a second layer, which is where the union sits") {
        // With two layers the tape ends in the hard union that folds the
        // second into the first, and an appended item belongs in front of it.
        // A soft blend is what makes the difference observable.
        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        clay_layer_id second = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "detail", &second) == CLAY_OK);

        auto add_soft = [&](clay_document* d, clay_layer_id layer, float x) {
            float r = 0.5f;
            clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
            REQUIRE(it != nullptr);
            const float pos[3] = {x, 0.0f, 0.0f};
            REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
            REQUIRE(clay_item_set_op(it, CLAY_OP_ADD) == CLAY_OK);
            REQUIRE(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, 0.25f) == CLAY_OK);
            REQUIRE(clay_layer_add_item(d, layer, it, nullptr) == CLAY_OK);
            clay_item_destroy(it);
        };
        add_soft(doc.d, second, 0.8f);
        field_signature(doc.d);
        add_soft(doc.d, second, 1.1f);

        Doc built;
        add_sphere(built, 1.0f, 0.0f);
        clay_layer_id built_second = 0;
        REQUIRE(clay_add_sdf_layer(built.d, "detail", &built_second) == CLAY_OK);
        add_soft(built.d, built_second, 0.8f);
        add_soft(built.d, built_second, 1.1f);
        CHECK(field_signature(doc.d) == field_signature(built.d));
    }
}

// Several readers can race the rebuild an append leaves pending. The append
// log is consumed by whichever of them gets there first, so a second rebuild
// must not apply it again — appending the same items twice would be a wrong
// field that no single-threaded test could produce.
//
// Mutating a document while another thread reads it was never supported and
// is not what this covers: the appends happen first, then the readers race.
TEST_CASE("tape cache: readers racing a pending append all get the same field") {
    // The probes, and the answer, computed on THIS thread: doctest's
    // assertion macros are not thread-safe, so the workers below compare and
    // count rather than REQUIRE — the same reason the reader test above
    // inlines its eval instead of calling field_signature.
    const int kProbes = 24;
    std::vector<float> pts(kProbes * 3);
    for (int i = 0; i < kProbes; ++i) {
        pts[i * 3 + 0] = -1.2f + 0.1f * static_cast<float>(i);
        pts[i * 3 + 1] = 0.11f * static_cast<float>(i % 7) - 0.3f;
        pts[i * 3 + 2] = 0.09f * static_cast<float>(i % 5) - 0.2f;
    }

    Doc oracle;
    add_sphere(oracle, 1.0f, 0.0f);
    for (int i = 1; i <= 3; ++i) add_sphere(oracle, 0.3f, 0.4f * static_cast<float>(i));
    std::vector<float> expected(kProbes);
    REQUIRE(clay_eval_points(oracle.d, nullptr, pts.data(), kProbes, expected.data(), nullptr) ==
            CLAY_OK);

    Doc doc;
    add_sphere(doc, 1.0f, 0.0f);
    field_signature(doc.d);  // compile, so the appends below have a prefix
    for (int i = 1; i <= 3; ++i) add_sphere(doc, 0.3f, 0.4f * static_cast<float>(i));
    // Deliberately not read here: the rebuild is left pending so the threads
    // race for it, and the append log must be consumed exactly once.

    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            std::vector<float> got(kProbes);
            for (int k = 0; k < 50; ++k) {
                if (clay_eval_points(doc.d, nullptr, pts.data(), kProbes, got.data(), nullptr) !=
                        CLAY_OK ||
                    got != expected)
                    mismatches.fetch_add(1);
            }
        });
    }
    for (std::thread& w : workers) w.join();
    CHECK(mismatches.load() == 0);
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

namespace {

// A culled grid evaluation, which is the read that goes through the CULL
// INDEX rather than through the tape — the cache `clay_eval_grid` consults
// per brick, and the one an append now extends instead of rebuilding.
std::vector<float> grid_signature(const clay_document* d) {
    clay_grid_query g;
    std::memset(&g, 0, sizeof(g));
    g.struct_size = sizeof(g);
    g.origin[0] = -1.1f;
    g.origin[1] = -1.1f;
    g.origin[2] = -1.1f;
    g.spacing = 0.13f;
    g.dims[0] = 18;
    g.dims[1] = 18;
    g.dims[2] = 18;
    const float lo[3] = {-1.2f, -1.2f, -1.2f};
    const float hi[3] = {1.2f, 1.2f, 1.2f};
    std::vector<float> v(static_cast<std::size_t>(g.dims[0]) * g.dims[1] * g.dims[2]);
    REQUIRE(clay_eval_grid(d, nullptr, &g, lo, hi, v.data(), nullptr, v.size()) == CLAY_OK);
    return v;
}

}  // namespace

TEST_CASE("cull index cache: an extended index reads as a rebuilt one") {
    // The index is cached per revision like the tape, and an append now
    // EXTENDS it rather than rebuilding — 2.42 ms to 0.13 ms at 50,000 items.
    // The oracle is a fresh document holding the same items: its first read
    // has no index to extend and so is always a full build, so an extension
    // that drifted shows as a disagreement with it.
    //
    // Bit-for-bit, not approximately: the index is a pure acceleration, so the
    // culled compile it feeds must make exactly the decisions a rebuilt one
    // makes and the values must be the same floats.
    auto built_with = [](int dabs) {
        Doc fresh;
        add_sphere(fresh, 1.0f, 0.0f);
        for (int i = 1; i <= dabs; ++i)
            add_sphere(fresh, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
        return grid_signature(fresh.d);
    };

    SUBCASE("every dab of a stroke") {
        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        const std::vector<float> base = grid_signature(doc.d);
        // A stroke: every append after the first extends the index the
        // previous one produced, which one dab never reaches.
        for (int i = 1; i <= 8; ++i) {
            CAPTURE(i);
            add_sphere(doc, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
            CHECK(grid_signature(doc.d) == built_with(i));
        }
        add_sphere(doc, 0.9f, 0.9f);
        CHECK(grid_signature(doc.d) != base);  // the stroke did move the field
    }

    SUBCASE("several appends absorbed by one read") {
        // The log accumulates across edits and is consumed by one extension;
        // one that took only the last item would pass a read-per-dab test.
        Doc a;
        add_sphere(a, 1.0f, 0.0f);
        grid_signature(a.d);  // build the index, so the appends below extend it
        for (int i = 1; i <= 5; ++i) add_sphere(a, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
        CHECK(grid_signature(a.d) == built_with(5));
    }

    SUBCASE("a general edit falls back to a rebuild") {
        // Anything that is not an append clears the log, and the index must
        // then be rebuilt rather than extended over a document that moved.
        // Undo is the sharpest case: it takes an item AWAY, so an index that
        // carried on extending would describe a document with one item too
        // many.
        Doc doc;
        REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
        add_sphere(doc, 1.0f, 0.0f);
        for (int i = 1; i <= 3; ++i) add_sphere(doc, 0.25f, -0.9f + 0.2f * static_cast<float>(i));
        const std::vector<float> three = grid_signature(doc.d);
        CHECK(three == built_with(3));

        int32_t undone = 0;
        REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
        CHECK(grid_signature(doc.d) == built_with(2));

        // And appending again after the undo still lands on the right answer,
        // which is what a log left dangling across the undo would break.
        add_sphere(doc, 0.25f, -0.9f + 0.2f * 3.0f);
        CHECK(grid_signature(doc.d) == three);
    }
}

namespace {

// A brick refill, which is the read the RESUMABLE path serves: when every
// brick asked for carries a seed from the same revision and the document has
// only been appended to since, the refill evaluates the appended items onto
// those seeds instead of the whole surviving edit list over every sample.
std::vector<float> refill_signature(clay_document* d) {
    constexpr int kDim = 8;
    constexpr float kVox = 0.05f;
    constexpr int kBricks = 8;
    const std::size_t per = static_cast<std::size_t>(kDim) * kDim * kDim;
    std::vector<clay_brick_request> reqs(kBricks);
    for (int i = 0; i < kBricks; ++i) {
        std::memset(&reqs[i], 0, sizeof(reqs[i]));
        // A row of bricks through the sphere's equator. The origin of a brick
        // IS key * dim * spacing, so the two cannot be chosen independently.
        const int kx = i - kBricks / 2;
        reqs[i].key[0] = kx;
        reqs[i].key[1] = -1;
        reqs[i].key[2] = -1;
        reqs[i].origin[0] = static_cast<float>(kx) * kDim * kVox;
        reqs[i].origin[1] = -1.0f * kDim * kVox;
        reqs[i].origin[2] = -1.0f * kDim * kVox;
        reqs[i].spacing = kVox;
        reqs[i].dims[0] = kDim;
        reqs[i].dims[1] = kDim;
        reqs[i].dims[2] = kDim;
        reqs[i].band = 3.0f * kVox;
    }
    std::vector<float> out(static_cast<std::size_t>(kBricks) * per, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), kBricks, out.data(), out.size(),
                                           nullptr, 0) == CLAY_OK);
    // The bricks must actually STRADDLE the surface, or every comparison below
    // is two readings of "far outside" agreeing with each other. This is the
    // check that would have caught a row of bricks placed off the shape.
    bool near_surface = false;
    for (float v : out) near_surface = near_surface || std::fabs(v) < 0.5f;
    REQUIRE(near_surface);
    return out;
}

}  // namespace

TEST_CASE("resumable refill: a resumed brick is the brick a full refill gives") {
    // The oracle is a FRESH document holding the same items: it has no seeds,
    // so its refill is always the full evaluation. Bit-for-bit, not
    // approximately — continuing a fold from the value it reached is the same
    // instructions over the same floats, so anything short of identity means
    // the suffix is not the suffix.
    auto built_with = [](int dabs) {
        Doc fresh;
        add_sphere(fresh, 1.0f, 0.0f);
        for (int i = 1; i <= dabs; ++i)
            add_sphere(fresh, 0.30f, -1.0f + 0.03f * static_cast<float>(i));
        return refill_signature(fresh.d);
    };

    SUBCASE("every dab of a stroke") {
        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        const std::vector<float> base = refill_signature(doc.d);  // seeds the store
        for (int i = 1; i <= 8; ++i) {
            CAPTURE(i);
            add_sphere(doc, 0.30f, -1.0f + 0.03f * static_cast<float>(i));
            const std::vector<float> got = refill_signature(doc.d);
            const std::vector<float> want = built_with(i);
            REQUIRE(got.size() == want.size());
            CHECK(std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) == 0);
        }
        // A dab entirely INSIDE the base sphere changes no distance at all --
        // min() is unchanged -- so the dabs above are placed to protrude, and
        // this holds that they did.
        CHECK(refill_signature(doc.d) != base);
    }

    SUBCASE("several appends between reads") {
        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        refill_signature(doc.d);
        for (int i = 1; i <= 4; ++i) add_sphere(doc, 0.30f, -1.0f + 0.03f * static_cast<float>(i));
        const std::vector<float> got = refill_signature(doc.d);
        const std::vector<float> want = built_with(4);
        CHECK(std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) == 0);
    }

    SUBCASE("an edit that is not an append falls back") {
        // Undo removes an item, so no seed can be carried across it.
        Doc doc;
        REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
        add_sphere(doc, 1.0f, 0.0f);
        for (int i = 1; i <= 3; ++i) add_sphere(doc, 0.30f, -1.0f + 0.03f * static_cast<float>(i));
        const std::vector<float> three = refill_signature(doc.d);
        CHECK(std::memcmp(three.data(), built_with(3).data(), three.size() * sizeof(float)) == 0);

        int32_t undone = 0;
        REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
        const std::vector<float> two = refill_signature(doc.d);
        CHECK(std::memcmp(two.data(), built_with(2).data(), two.size() * sizeof(float)) == 0);

        // And appending again after the undo still lands right.
        add_sphere(doc, 0.30f, -1.0f + 0.03f * 3.0f);
        const std::vector<float> again = refill_signature(doc.d);
        CHECK(std::memcmp(again.data(), three.data(), again.size() * sizeof(float)) == 0);
    }

    SUBCASE("colour is carried through the resumed path") {
        // A coloured walk folds a CTapeValue, so the seed carries the colour
        // the prefix reached as well as its distance. Continuing from the
        // distance alone would fold every combine against black.
        constexpr int kDim = 8;
        const std::size_t per = static_cast<std::size_t>(kDim) * kDim * kDim;
        auto refill_rgb = [&](clay_document* d, std::vector<float>& v, std::vector<float>& c) {
            clay_brick_request req;
            std::memset(&req, 0, sizeof(req));
            req.key[0] = 2;
            req.key[1] = -1;
            req.key[2] = -1;
            for (int a = 0; a < 3; ++a)
                req.origin[a] = static_cast<float>(req.key[a]) * kDim * 0.05f;
            req.spacing = 0.05f;
            req.dims[0] = req.dims[1] = req.dims[2] = kDim;
            req.band = 0.15f;
            v.assign(per, 0.0f);
            c.assign(per * 3, 0.0f);
            REQUIRE(clay_brick_cache_eval_requests(d, nullptr, &req, 1, v.data(), v.size(),
                                                   c.data(), c.size()) == CLAY_OK);
        };
        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        std::vector<float> v0, c0;
        refill_rgb(doc.d, v0, c0);  // seeds the store, with colour
        for (int i = 1; i <= 4; ++i) add_sphere(doc, 0.30f, 1.0f - 0.03f * static_cast<float>(i));
        std::vector<float> v, c;
        refill_rgb(doc.d, v, c);

        Doc oracle;
        add_sphere(oracle, 1.0f, 0.0f);
        for (int i = 1; i <= 4; ++i)
            add_sphere(oracle, 0.30f, 1.0f - 0.03f * static_cast<float>(i));
        std::vector<float> ov, oc;
        refill_rgb(oracle.d, ov, oc);

        CHECK(std::memcmp(v.data(), ov.data(), per * sizeof(float)) == 0);
        CHECK(std::memcmp(c.data(), oc.data(), per * 3 * sizeof(float)) == 0);
        // The stroke moved the field, so the agreement is not two readings of
        // an unchanged document.
        CHECK(std::memcmp(v.data(), v0.data(), per * sizeof(float)) != 0);
    }

    SUBCASE("a distance-only seed cannot serve a coloured refill") {
        // The store may hold a brick that was refilled WITHOUT colour. Serving
        // a coloured request from it would fold against black, so it falls back
        // and the answer is still right.
        constexpr int kDim = 8;
        const std::size_t per = static_cast<std::size_t>(kDim) * kDim * kDim;
        clay_brick_request req;
        std::memset(&req, 0, sizeof(req));
        req.key[0] = 2;
        req.key[1] = -1;
        req.key[2] = -1;
        for (int a = 0; a < 3; ++a) req.origin[a] = static_cast<float>(req.key[a]) * kDim * 0.05f;
        req.spacing = 0.05f;
        req.dims[0] = req.dims[1] = req.dims[2] = kDim;
        req.band = 0.15f;

        Doc doc;
        add_sphere(doc, 1.0f, 0.0f);
        std::vector<float> v(per);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, &req, 1, v.data(), v.size(), nullptr,
                                               0) == CLAY_OK);  // no colour stored
        add_sphere(doc, 0.30f, 0.95f);
        std::vector<float> v2(per), c2(per * 3);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, &req, 1, v2.data(), v2.size(),
                                               c2.data(), c2.size()) == CLAY_OK);
        Doc oracle;
        add_sphere(oracle, 1.0f, 0.0f);
        add_sphere(oracle, 0.30f, 0.95f);
        std::vector<float> ov(per), oc(per * 3);
        REQUIRE(clay_brick_cache_eval_requests(oracle.d, nullptr, &req, 1, ov.data(), ov.size(),
                                               oc.data(), oc.size()) == CLAY_OK);
        CHECK(std::memcmp(v2.data(), ov.data(), per * sizeof(float)) == 0);
        CHECK(std::memcmp(c2.data(), oc.data(), per * 3 * sizeof(float)) == 0);
    }
}
