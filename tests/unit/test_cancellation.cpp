#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "clay.h"
#include "clay/parallel/cancel.h"
#include "clay/parallel/thread_pool.h"

// Cancelling a long operation (c-abi spec: a long operation can be cancelled;
// a cancelled operation changes nothing; progress is readable without a
// callback).
//
// The device gate says why this exists: mask_extrude measures 4403 ms and
// consolidate 661 ms on the reference iPad, and every one of those was a
// synchronous call a host entered and could not leave.

using namespace clay;

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        // Enough items that a bake takes long enough to cancel mid-flight.
        for (int i = 0; i < 24; ++i) {
            const float r = 0.30f + 0.01f * static_cast<float>(i);
            clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
            REQUIRE(it != nullptr);
            const float pos[3] = {0.05f * static_cast<float>(i), 0.0f, 0.0f};
            REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
            clay_node_id id = 0;
            REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
            clay_item_destroy(it);
        }
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

struct Token {
    clay_cancel_token* t = clay_cancel_token_create();
    Token() = default;
    ~Token() { clay_cancel_token_destroy(t); }
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
};

clay_consolidation_params fine_params() {
    clay_consolidation_params p{};
    p.struct_size = sizeof(p);
    p.cell_size = 0.01f;  // fine enough that the bake is real work
    return p;
}

std::vector<float> probe(const clay_document* d) {
    const float pts[9] = {0, 0, 0, 0.3f, 0.1f, 0, 2, 2, 2};
    std::vector<float> out(3, 0.0f);
    REQUIRE(clay_eval_points(d, nullptr, pts, 3, out.data(), nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("cancel: a token cancelled before the call stops it promptly") {
    Doc doc;
    Token token;
    clay_cancel_token_cancel(token.t);
    CHECK(clay_cancel_token_cancelled(token.t) == 1);

    const std::vector<float> before = probe(doc.d);
    clay_consolidation_params p = fine_params();
    const clay_result r = clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr,
                                                             nullptr, nullptr, token.t);
    CHECK(r == CLAY_ERROR_CANCELLED);
    // And nothing moved.
    const std::vector<float> after = probe(doc.d);
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(after[i] == doctest::Approx(before[i]));
}

TEST_CASE("cancel: a cancelled consolidate leaves the document byte-identical") {
    // The guarantee, not an implementation note: the bake builds a volume and
    // installs it at the end, so a cancel is a discard. A host never has to
    // undo one.
    Doc doc;
    Token token;
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    std::size_t depth_before = 0;
    REQUIRE(clay_document_undo_state(doc.d, nullptr, &depth_before, nullptr) == CLAY_OK);

    clay_blob* before = nullptr;
    REQUIRE(clay_document_save_memory(doc.d, &before) == CLAY_OK);
    const std::vector<std::uint8_t> before_bytes(
        clay_blob_data(before), clay_blob_data(before) + clay_blob_size(before));
    clay_blob_destroy(before);

    clay_cancel_token_cancel(token.t);
    clay_consolidation_params p = fine_params();
    CHECK(clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr, nullptr, nullptr,
                                             token.t) == CLAY_ERROR_CANCELLED);

    clay_blob* after = nullptr;
    REQUIRE(clay_document_save_memory(doc.d, &after) == CLAY_OK);
    const std::vector<std::uint8_t> after_bytes(
        clay_blob_data(after), clay_blob_data(after) + clay_blob_size(after));
    clay_blob_destroy(after);

    CHECK(after_bytes == before_bytes);  // byte-identical, not merely equivalent

    std::size_t depth_after = 0;
    REQUIRE(clay_document_undo_state(doc.d, nullptr, &depth_after, nullptr) == CLAY_OK);
    CHECK(depth_after == depth_before);  // and not an undo step
}

TEST_CASE("cancel: a null token is exactly the call that was there before") {
    Doc doc;
    clay_consolidation_params p = fine_params();
    p.cell_size = 0.05f;  // coarse: this one is meant to finish
    const clay_result with_null = clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr,
                                                                     nullptr, nullptr, nullptr);
    Doc other;
    const clay_result sugar =
        clay_layer_consolidate(other.d, other.sdf, &p, nullptr, nullptr, nullptr);
    CHECK(with_null == sugar);
    CHECK(with_null == CLAY_OK);
}

TEST_CASE("cancel: cancelling from another thread ends a running operation") {
    Doc doc;
    Token token;
    const std::vector<float> before = probe(doc.d);

    // Cancel as soon as the operation reports itself running — which is the
    // only cross-thread-legal call in this ABI, and the reason the token exists
    // rather than a callback.
    std::atomic<bool> done{false};
    std::thread canceller([&] {
        for (int i = 0; i < 20000 && !done.load(); ++i) {
            clay_progress pr{};
            pr.struct_size = sizeof(pr);
            if (clay_cancel_token_progress(token.t, &pr) == CLAY_OK && pr.running) {
                clay_cancel_token_cancel(token.t);
                return;
            }
            std::this_thread::yield();
        }
        clay_cancel_token_cancel(token.t);  // belt and braces: the test must terminate
    });

    clay_consolidation_params p = fine_params();
    const clay_result r = clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr,
                                                             nullptr, nullptr, token.t);
    done.store(true);
    canceller.join();

    // Either it finished before the cancel landed or it was cancelled; both are
    // legitimate races. What must hold is that a CANCELLED one changed nothing.
    if (r == CLAY_ERROR_CANCELLED) {
        const std::vector<float> after = probe(doc.d);
        for (std::size_t i = 0; i < before.size(); ++i)
            CHECK(after[i] == doctest::Approx(before[i]));
    } else {
        CHECK(r == CLAY_OK);
    }
}

TEST_CASE("cancel: progress is readable, and idle is not a stale fraction") {
    Token token;
    clay_progress pr{};
    pr.struct_size = sizeof(pr);
    REQUIRE(clay_cancel_token_progress(token.t, &pr) == CLAY_OK);
    CHECK(pr.running == 0);
    CHECK(pr.fraction == doctest::Approx(0.0f));
    CHECK(pr.phase_count == 0);

    Doc doc;
    clay_consolidation_params p = fine_params();
    p.cell_size = 0.05f;
    REQUIRE(clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr, nullptr, nullptr,
                                               token.t) == CLAY_OK);
    // Finished: it reports idle rather than a stale 100%.
    REQUIRE(clay_cancel_token_progress(token.t, &pr) == CLAY_OK);
    CHECK(pr.running == 0);
}

TEST_CASE("cancel: a token is reusable after a cancel") {
    // A host holding one token per document must not pay an allocation per
    // cancel on the interactive path.
    Doc doc;
    Token token;
    clay_cancel_token_cancel(token.t);
    clay_consolidation_params p = fine_params();
    CHECK(clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr, nullptr, nullptr,
                                             token.t) == CLAY_ERROR_CANCELLED);

    clay_cancel_token_reset(token.t);
    CHECK(clay_cancel_token_cancelled(token.t) == 0);
    p.cell_size = 0.05f;
    CHECK(clay_layer_consolidate_cancellable(doc.d, doc.sdf, &p, nullptr, nullptr, nullptr,
                                             token.t) == CLAY_OK);
}

TEST_CASE("cancel: the token's calls tolerate null and are safe out of order") {
    // Safe before an operation, after one, and against a null handle: a host
    // driving this from a UI thread should not have to sequence it.
    clay_cancel_token_cancel(nullptr);
    clay_cancel_token_reset(nullptr);
    clay_cancel_token_destroy(nullptr);
    CHECK(clay_cancel_token_cancelled(nullptr) == 0);
    clay_progress pr{};
    pr.struct_size = sizeof(pr);
    CHECK(clay_cancel_token_progress(nullptr, &pr) == CLAY_ERROR_INVALID_ARGUMENT);

    Token token;
    CHECK(clay_cancel_token_progress(token.t, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_cancel_token_cancel(token.t);
    clay_cancel_token_cancel(token.t);  // twice is not an error
    clay_cancel_token_reset(token.t);
}

TEST_CASE("cancel: a cancelled parallel_for still JOINS") {
    // The hang, and the reason the token lives beside the pool. parallel_for
    // waits for `done >= num_tasks` and run() increments `done` only AFTER fn
    // returns — so a cancelled chunk must return NORMALLY. A body that threw
    // would skip the increment and the issuing thread would wait forever.
    //
    // If this regresses, the suite hangs rather than failing, which is exactly
    // why it is pinned.
    parallel::CancelToken token;
    std::atomic<std::size_t> entered{0};
    std::atomic<std::size_t> did_work{0};
    token.cancel();

    parallel::ThreadPool::instance().parallel_for(4096, 1, [&](std::size_t b, std::size_t e) {
        entered.fetch_add(1);
        if (token.cancelled()) return;  // return, never throw
        for (std::size_t i = b; i < e; ++i) did_work.fetch_add(1);
    });

    // Every chunk was still claimed and returned — that is the point: the pool
    // needs no cancellation concept, and the cost is one relaxed load per chunk.
    CHECK(entered.load() > 0);
    CHECK(did_work.load() == 0);
}
