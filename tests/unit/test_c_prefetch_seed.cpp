// Warming a cold brick window off the interaction thread (#306).
//
// A brick with no seed pays the whole surviving edit list, and that cost
// follows the size of the sculpt — which is what is left of #306 now that a
// dab whose bricks are warm is flat. The seed store belongs to the DOCUMENT
// rather than to a cache or a thread, and clay_brick_cache_eval_requests is
// free-threaded against a const document, so a host can pay that cost on a
// worker and hand the frame a warm window.
//
// What these cases hold is the two halves that make it usable: the seeds a
// worker leaves are visible to another thread's refill, and the values that
// refill produces are BIT-IDENTICAL to the ones the worker computed. A
// prefetch that returned "close enough" values would be a silent correctness
// bug in a performance feature, which is the worst place to put one.

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

// A worked sphere: a base plus `n` blended dabs spread over it, so a brick in
// the middle of the work has many surviving items rather than a few.
clay_layer_id worked_sphere(clay_document* doc, int n) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    float base = 1.0f;
    clay_item* b = clay_item_create(CLAY_PRIM_SPHERE, &base, 1);
    REQUIRE(clay_layer_add_item(doc, layer, b, nullptr) == CLAY_OK);
    clay_item_destroy(b);
    const float golden = 2.39996323f;
    for (int i = 0; i < n; ++i) {
        const float t = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - t * t));
        const float a = golden * static_cast<float>(i);
        const float p[3] = {r * std::cos(a), t, r * std::sin(a)};
        float rr = 0.06f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &rr, 1);
        REQUIRE(clay_item_set_position(it, p) == CLAY_OK);
        REQUIRE(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, 0.06f) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }
    return layer;
}

clay_brick_cache* make_cache() {
    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* c = clay_brick_cache_create(&cfg);
    REQUIRE(c != nullptr);
    return c;
}

std::vector<clay_brick_request> window(clay_brick_cache* cache, const float centre[3]) {
    const float lo[3] = {centre[0] - 0.2f, centre[1] - 0.2f, centre[2] - 0.2f};
    const float hi[3] = {centre[0] + 0.2f, centre[1] + 0.2f, centre[2] + 0.2f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    std::vector<clay_brick_request> reqs(4096);
    std::size_t count = reqs.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(remaining == 0);
    reqs.resize(count);
    return reqs;
}

clay_resume_stats stats_of(const clay_document* doc) {
    clay_resume_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    REQUIRE(clay_document_resume_stats(doc, &s) == CLAY_OK);
    return s;
}

}  // namespace

TEST_CASE("c abi: a window warmed on a worker is resumed by the next refill") {
    Doc d;
    worked_sphere(d.doc, 400);
    clay_brick_cache* cache = make_cache();
    const float centre[3] = {0.0f, 1.0f, 0.0f};
    const std::vector<clay_brick_request> reqs = window(cache, centre);
    REQUIRE(reqs.size() > 4);
    const std::size_t per = 8 * 8 * 8, total = reqs.size() * per;

    const clay_resume_stats before = stats_of(d.doc);

    // The WORKER pays the cold walk. The document is not edited while it runs,
    // which is the condition clay_brick_cache_eval_requests states.
    std::vector<float> warm(total);
    std::atomic<clay_result> worker_result{CLAY_ERROR_INVALID_ARGUMENT};
    std::thread worker([&] {
        worker_result.store(clay_brick_cache_eval_requests(
            d.doc, "cpu", reqs.data(), reqs.size(), warm.data(), total, nullptr, 0));
    });
    worker.join();
    REQUIRE(worker_result.load() == CLAY_OK);

    const clay_resume_stats warmed = stats_of(d.doc);
    // The worker found nothing to resume from and left a seed per brick.
    CHECK(warmed.refilled_bricks - before.refilled_bricks == reqs.size());
    CHECK(warmed.entries >= reqs.size());

    // The interaction thread now refills the SAME requests and resumes.
    std::vector<float> frame(total);
    REQUIRE(clay_brick_cache_eval_requests(d.doc, "cpu", reqs.data(), reqs.size(), frame.data(),
                                           total, nullptr, 0) == CLAY_OK);
    const clay_resume_stats after = stats_of(d.doc);
    CHECK(after.resumed_bricks - warmed.resumed_bricks == reqs.size());
    CHECK(after.refilled_bricks == warmed.refilled_bricks);  // nothing walked again

    // AND THE VALUES ARE THE SAME ONES. This is the half that matters: a
    // prefetch that produced "close enough" values would put a silent
    // correctness bug inside a performance feature.
    CHECK(frame == warm);

    clay_brick_cache_destroy(cache);
}

TEST_CASE("c abi: warming does not change what a refill answers") {
    // The seed is a pure performance cache, so a warmed document and one that
    // never saw a worker must produce the same bricks. Two documents built from
    // the same items, one warmed and one not.
    Doc warmed_doc;
    Doc plain_doc;
    worked_sphere(warmed_doc.doc, 300);
    worked_sphere(plain_doc.doc, 300);

    clay_brick_cache* c1 = make_cache();
    clay_brick_cache* c2 = make_cache();
    const float centre[3] = {0.0f, 1.0f, 0.0f};
    const std::vector<clay_brick_request> r1 = window(c1, centre);
    const std::vector<clay_brick_request> r2 = window(c2, centre);
    REQUIRE(r1.size() == r2.size());
    const std::size_t per = 8 * 8 * 8, total = r1.size() * per;

    // One document is warmed twice over — once by a worker, once by the frame.
    std::vector<float> scratch(total), warmed(total), plain(total);
    std::thread worker([&] {
        clay_brick_cache_eval_requests(warmed_doc.doc, "cpu", r1.data(), r1.size(),
                                       scratch.data(), total, nullptr, 0);
    });
    worker.join();
    REQUIRE(clay_brick_cache_eval_requests(warmed_doc.doc, "cpu", r1.data(), r1.size(),
                                           warmed.data(), total, nullptr, 0) == CLAY_OK);
    // The other is refilled once, cold, having never been touched.
    REQUIRE(clay_brick_cache_eval_requests(plain_doc.doc, "cpu", r2.data(), r2.size(),
                                           plain.data(), total, nullptr, 0) == CLAY_OK);

    CHECK(warmed == plain);
    CHECK(stats_of(warmed_doc.doc).resumed_bricks > 0);   // the warmed one resumed
    CHECK(stats_of(plain_doc.doc).resumed_bricks == 0);   // the plain one did not

    clay_brick_cache_destroy(c1);
    clay_brick_cache_destroy(c2);
}

TEST_CASE("c abi: several workers may warm disjoint windows at once") {
    // The contract says any number of threads against one const document. A
    // host warming the neighbourhood a brush is about to enter will want more
    // than one, and the seeds they leave must all be usable.
    Doc d;
    worked_sphere(d.doc, 300);
    clay_brick_cache* cache = make_cache();
    const float centres[3][3] = {
        {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    std::vector<std::vector<clay_brick_request>> reqs;
    for (const auto& c : centres) reqs.push_back(window(cache, c));
    const std::size_t per = 8 * 8 * 8;

    std::vector<std::vector<float>> warm(reqs.size());
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        warm[i].resize(reqs[i].size() * per);
        workers.emplace_back([&, i] {
            clay_brick_cache_eval_requests(d.doc, "cpu", reqs[i].data(), reqs[i].size(),
                                           warm[i].data(), warm[i].size(), nullptr, 0);
        });
    }
    for (std::thread& w : workers) w.join();

    // Every window now resumes, and every value matches what its worker got.
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const clay_resume_stats before = stats_of(d.doc);
        std::vector<float> frame(warm[i].size());
        REQUIRE(clay_brick_cache_eval_requests(d.doc, "cpu", reqs[i].data(), reqs[i].size(),
                                               frame.data(), frame.size(), nullptr,
                                               0) == CLAY_OK);
        const clay_resume_stats after = stats_of(d.doc);
        CHECK(after.resumed_bricks - before.resumed_bricks == reqs[i].size());
        CHECK(frame == warm[i]);
    }
    clay_brick_cache_destroy(cache);
}
