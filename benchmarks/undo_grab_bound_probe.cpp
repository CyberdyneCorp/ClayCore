// What undoing ONE Move segment costs a host, against the length of the chain
// it sits on (issue #639). NOT a gated benchmark.
//
// THE SHAPE. A sphere node, `chain` grabs spread around its equator -- what a
// long Move session leaves on a node -- and one small grab on its pole, put at
// the HEAD of the chain as a Move puts it. The probe undoes that one grab and
// does what the header tells a host to: hand clay_document_undo_bound's box to
// clay_brick_cache_mark_dirty and refill what it marked. Then it redoes, and
// refills again, so every sample starts from the same document.
//
// WHAT IT REPORTS, per run: the bricks the undo marked -- a COUNT, the same on
// every machine, and the thing issue #639 is about -- and the median wall time
// of the undo (undo_bound + mark + refill) over `samples` repeats after five
// discarded ones, with that time divided by the brick count. The first is the
// node-size factor, the last is the per-brick price, which still grows with
// the chain after this change and is reported so that it is not hidden.
//
// Drives the C ABI only, so one source builds against any revision:
//
//   undo_grab_bound_probe [chain=10] [samples=200] [node_radius=1.5]
//
// Exits non-zero if the undo does not report a finite box, so it cannot
// measure a refill of nothing.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clay.h"

namespace {

constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;

void check(clay_result r, const char* what) {
    if (r == CLAY_OK) return;
    std::fprintf(stderr, "%s failed: %d %s\n", what, static_cast<int>(r), clay_last_error());
    std::exit(1);
}

struct Refill {
    std::vector<clay_brick_request> reqs = std::vector<clay_brick_request>(256);
    std::vector<float> values = std::vector<float>(256 * kSamples);
    std::vector<std::int32_t> results = std::vector<std::int32_t>(256);

    // Drain, evaluate, submit until nothing is dirty; returns bricks evaluated.
    std::size_t run(clay_brick_cache* cache, const clay_document* doc) {
        std::size_t evaluated = 0;
        for (;;) {
            std::size_t count = reqs.size(), remaining = 0;
            check(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining), "take");
            if (count == 0) break;
            check(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                                 count * kSamples, nullptr, 0),
                  "eval");
            std::size_t accepted = 0;
            check(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                          count * kSamples, nullptr, 0, results.data(), &accepted),
                  "submit");
            evaluated += count;
            if (remaining == 0) break;
        }
        return evaluated;
    }
};

void add_grab(clay_document* d, clay_layer_id layer, clay_node_id node, const float p[8]) {
    check(clay_layer_add_deformer(d, layer, node, CLAY_DEFORM_GRAB, p, 8, CLAY_EASE_LINEAR, 1),
          "add grab");
}

// The document: a sphere, `chain` grabs around its equator, one on its pole.
clay_document* build(int chain, float node_r, clay_layer_id* out_layer) {
    clay_document* d = clay_document_create();
    check(clay_add_sdf_layer(d, "body", out_layer), "layer");
    check(clay_document_enable_undo(d), "undo");
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &node_r, 1);
    clay_node_id node = 0;
    check(clay_layer_add_item(d, *out_layer, item, &node), "item");
    clay_item_destroy(item);
    for (int i = 0; i < chain; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / static_cast<float>(chain);
        const float c = std::cos(t), s = std::sin(t);
        const float p[8] = {node_r * c, node_r * s, 0.0f, 0.2f, 0.08f * c, 0.08f * s, 0.0f, 0.0f};
        add_grab(d, *out_layer, node, p);
    }
    const float pole[8] = {0.0f, 0.0f, node_r, 0.15f, 0.0f, 0.0f, 0.08f, 0.0f};
    add_grab(d, *out_layer, node, pole);
    return d;
}

// One step through the reporting entry point, marked and refilled.
std::size_t step(clay_document* d, clay_brick_cache* cache, Refill* refill, bool undo) {
    float lo[3], hi[3];
    std::int32_t done = 0, has = 0, inf = 0;
    check(undo ? clay_document_undo_bound(d, &done, lo, hi, &has, &inf)
               : clay_document_redo_bound(d, &done, lo, hi, &has, &inf),
          undo ? "undo" : "redo");
    if (!done || !has || inf) {
        std::fprintf(stderr, "the step reported no finite box\n");
        std::exit(1);
    }
    check(clay_brick_cache_mark_dirty(cache, lo, hi), "mark");
    return refill->run(cache, d);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    const int chain = argc > 1 ? std::atoi(argv[1]) : 10;
    const int samples = argc > 2 ? std::max(1, std::atoi(argv[2])) : 200;
    const float node_r = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 1.5f;

    clay_layer_id layer = 0;
    clay_document* d = build(chain, node_r, &layer);
    clay_brick_config cfg;
    cfg.struct_size = sizeof cfg;
    check(clay_brick_config_defaults(&cfg), "config");
    cfg.dim = kDim;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    Refill refill;
    check(clay_brick_cache_mark_dirty_layer(cache, d, layer), "mark layer");
    refill.run(cache, d);

    std::vector<double> undo_ms;
    std::size_t bricks = 0;
    for (int i = 0; i < samples + 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        bricks = step(d, cache, &refill, /*undo=*/true);
        const auto t1 = std::chrono::steady_clock::now();
        if (i >= 5) undo_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        step(d, cache, &refill, /*undo=*/false);
    }
    const double p50 = median(undo_ms);
    std::printf("chain=%d node_r=%.2f samples=%d bricks=%zu undo_p50_ms=%.3f per_brick_us=%.2f\n",
                chain, static_cast<double>(node_r), samples, bricks, p50,
                1000.0 * p50 / static_cast<double>(bricks));
    clay_brick_cache_destroy(cache);
    clay_document_destroy(d);
    return 0;
}
