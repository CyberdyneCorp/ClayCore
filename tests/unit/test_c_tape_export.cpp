#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "clay.h"
#include "clay/kernel/tape.h"
#include "clay/version.h"

// The compiled tape across the C boundary (c-abi spec: the compiled tape is
// exportable). Issue #43 item 5: docs/06 tells hosts to "upload instrs /
// params / blob as the three tape buffers" and ships a Metal example doing
// exactly that — but a C-ABI consumer had no way to obtain them, so the
// anti-drift story was reachable from C++ and Swift and not from the ABI that
// "is the only surface a packaged consumer has".
//
// The tests that matter here are not the buffer contents. They are:
//
//   1. The exported tape EVALUATES to what the library evaluates, through
//      ctape_eval — the same entry point the published headers give a host.
//   2. Editing the document cannot invalidate an export. The design picked a
//      snapshot handle precisely so this test can only pass, rather than
//      picking borrowed pointers and hoping every host notices every edit.

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

struct Tape {
    clay_tape* t = nullptr;
    ~Tape() { clay_tape_release(t); }
    Tape() = default;
    Tape(const Tape&) = delete;
    Tape& operator=(const Tape&) = delete;
};

// blend_k > 0 makes this a SMOOTH boolean, which is the case that matters
// here: the drift docs/06 exists to prevent was a hand-written quadratic smin
// of support k where the engine uses 4k, so a comparison over hard unions
// alone would have missed the bug this whole document is about.
clay_node_id add_sphere(Doc& doc, float r, float x, float y, float z,
                        std::int32_t op = CLAY_OP_ADD, float blend_k = 0.0f) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_op(it, op) == CLAY_OK);
    if (blend_k > 0.0f)
        REQUIRE(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, blend_k) == CLAY_OK);
    const float rgb[3] = {0.7f, 0.2f, 0.4f};
    REQUIRE(clay_item_set_color(it, rgb) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

// Evaluate an exported tape exactly as a host does: ctape_eval over the three
// borrowed buffers, with nothing from the library but the bytes.
clay::kernel::CTapeValue eval_exported(const clay_tape* tape, float x, float y, float z) {
    std::size_t ni = 0, np = 0, nb = 0;
    const clay_tape_instr* instrs = clay_tape_instrs(tape, &ni);
    const float* params = clay_tape_params(tape, &np);
    const float* blob = clay_tape_blob(tape, &nb);
    return clay::kernel::ctape_eval(
        reinterpret_cast<const clay::kernel::CTapeInstr*>(instrs), static_cast<int>(ni), params,
        blob, clay::kernel::cf3(x, y, z));
}

std::vector<std::array<float, 3>> probes() {
    std::vector<std::array<float, 3>> out;
    for (int i = -3; i <= 3; ++i)
        for (int j = -3; j <= 3; ++j)
            for (int k = -3; k <= 3; ++k)
                out.push_back({static_cast<float>(i) * 0.21f, static_cast<float>(j) * 0.21f,
                               static_cast<float>(k) * 0.21f});
    return out;
}

}  // namespace

TEST_CASE("tape export: a host evaluates our field with our kernels") {
    Doc doc;
    add_sphere(doc, 0.4f, -0.15f, 0, 0);
    add_sphere(doc, 0.35f, 0.2f, 0.1f, 0, CLAY_OP_ADD, 0.12f);      // smooth union
    add_sphere(doc, 0.2f, 0, 0.3f, 0.1f, CLAY_OP_SUBTRACT, 0.08f);  // smooth subtract

    Tape tape;
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &tape.t) == CLAY_OK);
    std::size_t ni = 0;
    REQUIRE(clay_tape_instrs(tape.t, &ni) != nullptr);
    CHECK(ni > 0);

    // The point of the whole change: the same numbers, through the evaluator a
    // host compiles from the published headers.
    for (const auto& p : probes()) {
        float expect_d = 0.0f, expect_rgb[3] = {0, 0, 0};
        REQUIRE(clay_eval_points(doc.d, nullptr, p.data(), 1, &expect_d, expect_rgb) == CLAY_OK);
        const clay::kernel::CTapeValue got = eval_exported(tape.t, p[0], p[1], p[2]);
        CHECK(got.d == doctest::Approx(expect_d).epsilon(1e-5));
        CHECK(got.color.x == doctest::Approx(expect_rgb[0]).epsilon(1e-4));
        CHECK(got.color.y == doctest::Approx(expect_rgb[1]).epsilon(1e-4));
        CHECK(got.color.z == doctest::Approx(expect_rgb[2]).epsilon(1e-4));
    }

    SUBCASE("and gets what the buffers cannot tell it") {
        std::int32_t exact = -1;
        float lip = 0.0f, step = 0.0f, lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        std::uint64_t revision = 0;
        REQUIRE(clay_tape_info(tape.t, &exact, &lip, &step, lo, hi, &revision) == CLAY_OK);
        CHECK((exact == 0 || exact == 1));
        CHECK(lip >= 1.0f);
        // published rather than left to the host to recompute: a host that
        // guesses its step scale draws a wrong frame
        CHECK(step == doctest::Approx(1.0f / std::max(lip, 1.0f)));
        CHECK(revision > 0);
        // the bounds are what a raymarcher clips against, and they contain the
        // geometry
        for (int a = 0; a < 3; ++a) CHECK(lo[a] < hi[a]);
        const float centre[3] = {-0.15f, 0, 0};
        for (int a = 0; a < 3; ++a) {
            CHECK(lo[a] <= centre[a]);
            CHECK(hi[a] >= centre[a]);
        }
        // every out pointer is optional
        CHECK(clay_tape_info(tape.t, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) ==
              CLAY_OK);
    }

    SUBCASE("and can tell the encoding it was built for") {
        const clay::Version v = clay::version();
        const std::uint32_t expect = static_cast<std::uint32_t>(v.major) * 1000000u +
                                     static_cast<std::uint32_t>(v.minor) * 1000u +
                                     static_cast<std::uint32_t>(v.patch);
        CHECK(clay_tape_encoding_version() == expect);
    }
}

// The lifetime rule, tested directly. This is the test the design exists to
// make trivial: with borrowed pointers into the document's cache it would be a
// use-after-free, and with a copy-out it would be a race between two calls.
TEST_CASE("tape export: an edit cannot invalidate an export") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);

    Tape before;
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &before.t) == CLAY_OK);
    std::uint64_t rev_before = 0;
    REQUIRE(clay_tape_info(before.t, nullptr, nullptr, nullptr, nullptr, nullptr, &rev_before) ==
            CLAY_OK);
    const float probe[3] = {0.5f, 0.0f, 0.0f};
    const float d_before = eval_exported(before.t, probe[0], probe[1], probe[2]).d;

    // Edit hard enough to force a recompile — and to change the field at the
    // probe, so a stale export is distinguishable from a fresh one.
    for (int i = 0; i < 8; ++i)
        add_sphere(doc, 0.3f, 0.5f + static_cast<float>(i) * 0.1f, 0, 0);

    // Still readable, still the OLD field: an export is a snapshot.
    std::size_t ni = 0;
    REQUIRE(clay_tape_instrs(before.t, &ni) != nullptr);
    CHECK(eval_exported(before.t, probe[0], probe[1], probe[2]).d ==
          doctest::Approx(d_before));

    Tape after;
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &after.t) == CLAY_OK);
    std::uint64_t rev_after = 0;
    REQUIRE(clay_tape_info(after.t, nullptr, nullptr, nullptr, nullptr, nullptr, &rev_after) ==
            CLAY_OK);
    // staleness is an integer comparison, not a comparison of buffers
    CHECK(rev_after != rev_before);
    CHECK(eval_exported(after.t, probe[0], probe[1], probe[2]).d < d_before);

    // and the older export is STILL valid after the newer one exists, which is
    // the property a refcounted snapshot gives and a cache pointer does not
    CHECK(eval_exported(before.t, probe[0], probe[1], probe[2]).d ==
          doctest::Approx(d_before));
}

TEST_CASE("tape export: a culled tape agrees inside its region") {
    Doc doc;
    add_sphere(doc, 0.3f, 0, 0, 0);
    add_sphere(doc, 0.3f, 4.0f, 0, 0);  // far away: the cull should drop it

    const float lo[3] = {-0.6f, -0.6f, -0.6f}, hi[3] = {0.6f, 0.6f, 0.6f};
    Tape culled, whole;
    REQUIRE(clay_tape_export(doc.d, lo, hi, &culled.t) == CLAY_OK);
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &whole.t) == CLAY_OK);

    std::size_t culled_n = 0, whole_n = 0;
    clay_tape_instrs(culled.t, &culled_n);
    clay_tape_instrs(whole.t, &whole_n);
    CHECK(culled_n < whole_n);  // the far sphere was dropped

    // and inside the region the two agree, which is what makes a culled tape
    // safe to draw a region with
    for (int i = -2; i <= 2; ++i)
        for (int j = -2; j <= 2; ++j) {
            const float x = static_cast<float>(i) * 0.2f, y = static_cast<float>(j) * 0.2f;
            CHECK(eval_exported(culled.t, x, y, 0.0f).d ==
                  doctest::Approx(eval_exported(whole.t, x, y, 0.0f).d).epsilon(1e-5));
        }

    SUBCASE("and the region follows clay_eval_grid's rules") {
        clay_tape* out = nullptr;
        CHECK(clay_tape_export(doc.d, lo, nullptr, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_tape_export(doc.d, nullptr, hi, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        const float inverted_lo[3] = {1, 1, 1}, inverted_hi[3] = {-1, -1, -1};
        CHECK(clay_tape_export(doc.d, inverted_lo, inverted_hi, &out) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        const float nan_lo[3] = {std::nanf(""), 0, 0};
        CHECK(clay_tape_export(doc.d, nan_lo, hi, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(out == nullptr);
        CHECK(clay_tape_export(nullptr, nullptr, nullptr, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_tape_export(doc.d, nullptr, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("tape export: exporting does not disturb a concurrent read") {
    Doc doc;
    for (int i = 0; i < 6; ++i) add_sphere(doc, 0.25f, static_cast<float>(i) * 0.2f, 0, 0);

    // "A document stays readable from several threads at once" is an existing
    // requirement; exporting must not be the call that breaks it.
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int t = 0; t < 4; ++t)
        workers.emplace_back([&doc, &failures] {
            for (int i = 0; i < 40; ++i) {
                clay_tape* tape = nullptr;
                if (clay_tape_export(doc.d, nullptr, nullptr, &tape) != CLAY_OK) ++failures;
                std::size_t n = 0;
                if (tape && clay_tape_instrs(tape, &n) == nullptr) ++failures;
                clay_tape_release(tape);
                const float p[3] = {0.3f, 0.1f, 0.0f};
                float d = 0.0f;
                if (clay_eval_points(doc.d, nullptr, p, 1, &d, nullptr) != CLAY_OK) ++failures;
            }
        });
    for (std::thread& w : workers) w.join();
    CHECK(failures.load() == 0);

    // releasing a null handle is a no-op, as clay_brick_cache_destroy is
    clay_tape_release(nullptr);
}
