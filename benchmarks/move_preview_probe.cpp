// What the pre-0.72.0 way of drawing a live Move drag actually costs.
// NOT a gated benchmark.
//
// THE CLAIM UNDER TEST is one this repository makes in its own header, above
// `clay_sdf_move_preview_document` (bindings/c/clay.h, ABI 0.72.0, issue #388):
//
//     Without it a C host had to write each resolved grab onto the layer with
//     clay_layer_add_deformer, sample, and then UNDO every one of them inside
//     the same segment -- two document mutations and a full undo round-trip per
//     pointer event, to draw something the transaction was already holding.
//
// That sentence argues a host off a pattern and carries NO NUMBER. It was
// written as though the cost were self-evident. It is not: an undo of two edits
// on a small dirty region could be a rounding error, in which case the sentence
// is talking a host into a lifetime-sensitive refactor for nothing. A real host
// (ClaySpaceDesktop) is on the old pattern today and correctly declined to
// change on the strength of a comment, which is what this file answers.
//
// THE DESIGN. One drag, driven twice over the same path with the same anchor,
// the same radius and the same refill:
//
//   A (pre-0.72.0)  per pointer event: add_deformer per resolved grab, inside
//                   one undo group -> mark_dirty -> drain the cache against the
//                   REAL document -> clay_document_undo to take it all back.
//
//   B (0.72.0+)     per pointer event: clay_sdf_move_update -> mark_dirty ->
//                   drain the cache against clay_sdf_move_preview_document.
//                   The document is never touched, so there is nothing to undo.
//
// WHAT MAKES IT A FAIR COMPARISON, and where it is deliberately unfair:
//
//   - The refill is the same work. Both arms mark the same box and drain the
//     same requests, and the probe FAILS if the two arms do not refill an equal
//     number of bricks. Without that the difference could be region size rather
//     than pattern, which is the trap `a-drag-cannot-see-its-own-invalidation`
//     records: hold the brick count equal, and the delta is per-pattern cost.
//
//   - Arm A gets its grabs for free. A pre-0.72.0 host resolved them itself
//     (clay_layer_move_surface_preview); here they are read out of a
//     transaction OUTSIDE A's timed region, and A is not charged for the
//     `clay_sdf_move_update` that produced them -- work arm B pays for inside
//     its own timing. That biases the result TOWARD A, which is the safe
//     direction for a claim this file exists to support. If A still loses, it
//     loses by at least the margin reported.
//
//   - Order alternates by repeat, so neither arm always runs on a colder cache.
//
// THE CORRECTNESS PRECONDITION, asserted rather than assumed: at the end of a
// drag both arms must have put the same field in front of the host. The probe
// samples the preview and the arm-A document at matched points and exits
// non-zero if they disagree beyond fp16 brick tolerance. A performance number
// from two arms that drew different things would be meaningless, and "the
// faster one was also wrong" is exactly the shape this repo keeps finding.
//
// Exits non-zero if either invariant fails, so it cannot pass by measuring
// nothing.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

// -- the fixture ------------------------------------------------------------
//
// A blockout form: overlapping spheres along an arc, blended, so a drag of
// radius 0.4 reaches a handful of them and the surface it moves is real. A
// single sphere would let one grab reach everything and would not exercise the
// per-node chain work either pattern pays.
constexpr int kStamps = 200;
constexpr float kVoxel = 0.05f;
constexpr int kBrickDim = 8;
constexpr int kBand = 2;
constexpr float kDragRadius = 0.4f;
constexpr int kEvents = 60;      // pointer events in one drag
constexpr int kRepeats = 4;      // -> 240 timed samples per arm

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build(Doc* out) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    for (int i = 0; i < kStamps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kStamps - 1);
        const float angle = t * 3.14159265f;
        const float radius = 0.22f;
        clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
        if (!item) {
            clay_document_destroy(doc);
            return false;
        }
        const float pos[3] = {std::cos(angle) * 1.2f, std::sin(angle) * 0.6f, 0.0f};
        clay_item_set_position(item, pos);
        clay_item_set_op(item, CLAY_OP_ADD);
        clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.05f);
        clay_node_id node = 0;
        const clay_result r = clay_layer_add_item(doc, layer, item, &node);
        clay_item_destroy(item);
        if (r != CLAY_OK) {
            clay_document_destroy(doc);
            return false;
        }
    }
    out->doc = doc;
    out->layer = layer;
    return true;
}

clay_brick_cache* make_cache() {
    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.dim = kBrickDim;
    cfg.voxel_size = kVoxel;
    cfg.band_voxels = kBand;
    cfg.memory_budget = 0;
    return clay_brick_cache_create(&cfg);
}

// Drain every dirty brick against `against`, which is the real document for arm
// A and the transaction's preview for arm B. Returns bricks refilled, or -1.
long drain(clay_brick_cache* cache, const clay_document* against) {
    long refilled = 0;
    std::vector<clay_brick_request> reqs(512);
    std::vector<float> values;
    for (;;) {
        std::size_t count = reqs.size();
        std::size_t remaining = 0;
        if (clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) != CLAY_OK)
            return -1;
        if (count == 0) break;
        const std::size_t per = static_cast<std::size_t>(kBrickDim) * kBrickDim * kBrickDim;
        values.assign(count * per, 0.0f);
        if (clay_brick_cache_eval_requests(against, "cpu", reqs.data(), count, values.data(),
                                           values.size(), nullptr, 0) != CLAY_OK)
            return -1;
        std::size_t accepted = 0;
        if (clay_brick_cache_submit(cache, reqs.data(), count, values.data(), values.size(),
                                    nullptr, 0, nullptr, &accepted) != CLAY_OK)
            return -1;
        refilled += static_cast<long>(count);
        if (remaining == 0) break;
    }
    return refilled;
}

struct Grab {
    clay_node_id node;
    float centre[3];
    float radius;
    float displacement[3];
    int32_t ease;
    int32_t front_only;
};

// The grabs the transaction resolved for this frame, read out so arm A can
// write them by hand. Outside A's timed region on purpose -- see the header.
bool read_grabs(const clay_sdf_move_tx* tx, const std::vector<clay_node_id>& nodes,
                std::vector<Grab>* out) {
    out->clear();
    for (const clay_node_id node : nodes) {
        std::size_t n = 0;
        if (clay_sdf_move_preview_grab_count(tx, node, &n) != CLAY_OK) continue;
        for (std::size_t i = 0; i < n; ++i) {
            Grab g{};
            g.node = node;
            if (clay_sdf_move_preview_grab(tx, node, i, g.centre, &g.radius, g.displacement,
                                           &g.ease, &g.front_only) != CLAY_OK)
                return false;
            out->push_back(g);
        }
    }
    return true;
}

// -- the correctness precondition -------------------------------------------
//
// Both arms must put the SAME field in front of the host, or the timing above
// compares two things that are not alternatives.
//
// Parameterised by TWO displacements on purpose. With both equal it asks the
// real question: does writing the resolved grabs onto the document reproduce
// what the preview showed? With them different it becomes the CALIBRATION --
// a drag that really is wrong by the given amount, which the same code path
// must report as a disagreement. A check that has not been shown to fail is
// not evidence, and this file exists because of a claim nobody had checked.
//
// Sampled through clay_eval_points rather than through the brick cache, so this
// compares the FIELD the two patterns produce and not the fp16 lattice both
// would quantise it into.
//
// Returns the largest absolute disagreement, or a negative value on failure.
double field_disagreement(const float preview_total[3], const float applied_total[3]) {
    const float anchor[3] = {0.0f, 0.6f, 0.35f};

    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kDragRadius;
    mp.ease = 0;
    mp.front_only = 0;

    // Probes through the dragged region, so the comparison covers where the
    // warp acts rather than one convenient point.
    std::vector<float> pts;
    for (int ix = 0; ix < 9; ++ix)
        for (int iy = 0; iy < 9; ++iy)
            for (int iz = 0; iz < 9; ++iz) {
                pts.push_back(anchor[0] + applied_total[0] * 0.5f + (ix - 4) * 0.12f);
                pts.push_back(anchor[1] + applied_total[1] * 0.5f + (iy - 4) * 0.12f);
                pts.push_back(anchor[2] + applied_total[2] * 0.5f + (iz - 4) * 0.12f);
            }
    const std::size_t n = pts.size() / 3;

    // Side B: the preview, on its own document, at preview_total.
    Doc db;
    if (!build(&db)) return -1.0;
    clay_sdf_move_tx* txb = clay_sdf_move_begin(db.doc, db.layer, anchor, &mp, nullptr);
    if (!txb) {
        clay_document_destroy(db.doc);
        return -1.0;
    }
    clay_sculpt_dirty dirty;
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    std::vector<float> b_vals(n, 0.0f);
    bool ok = clay_sdf_move_update(txb, preview_total, &dirty) == CLAY_OK;
    const clay_document* preview = ok ? clay_sdf_move_preview_document(txb) : nullptr;
    ok = ok && preview &&
         clay_eval_points(preview, "cpu", pts.data(), n, b_vals.data(), nullptr) == CLAY_OK;
    clay_sdf_move_cancel(txb);
    clay_sdf_move_destroy(txb);
    clay_document_destroy(db.doc);
    if (!ok) return -1.0;

    // Side A: the same grabs written onto a real document, at applied_total.
    Doc da;
    if (!build(&da)) return -1.0;
    clay_document_enable_undo(da.doc);
    clay_sdf_move_tx* txa = clay_sdf_move_begin(da.doc, da.layer, anchor, &mp, nullptr);
    if (!txa) {
        clay_document_destroy(da.doc);
        return -1.0;
    }
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    if (clay_sdf_move_update(txa, applied_total, &dirty) != CLAY_OK) {
        clay_sdf_move_destroy(txa);
        clay_document_destroy(da.doc);
        return -1.0;
    }
    std::size_t node_count = 0;
    clay_sdf_move_preview_nodes(txa, nullptr, 0, &node_count);
    std::vector<clay_node_id> nodes(node_count);
    if (node_count) clay_sdf_move_preview_nodes(txa, nodes.data(), nodes.size(), &node_count);
    std::vector<Grab> grabs;
    bool authored = read_grabs(txa, nodes, &grabs) && !grabs.empty();
    if (authored) {
        clay_document_begin_undo_group(da.doc);
        for (const Grab& g : grabs) {
            const float params[8] = {g.centre[0],       g.centre[1],
                                     g.centre[2],       g.radius,
                                     g.displacement[0], g.displacement[1],
                                     g.displacement[2], static_cast<float>(g.front_only)};
            if (clay_layer_add_deformer(da.doc, da.layer, g.node, CLAY_DEFORM_GRAB, params, 8,
                                        g.ease, /*at_front=*/1) != CLAY_OK)
                authored = false;
        }
        clay_document_end_undo_group(da.doc);
    }
    // The transaction must be gone before the document is sampled: arm A's
    // whole point is that the grabs live on the DOCUMENT, not in a preview.
    clay_sdf_move_cancel(txa);
    clay_sdf_move_destroy(txa);

    std::vector<float> a_vals(n, 0.0f);
    const bool eval_ok =
        authored &&
        clay_eval_points(da.doc, "cpu", pts.data(), n, a_vals.data(), nullptr) == CLAY_OK;
    clay_document_destroy(da.doc);
    if (!eval_ok) return -1.0;

    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a_vals[i]) -
                                          static_cast<double>(b_vals[i])));
    return worst;
}

}  // namespace

int main() {
    std::printf("move_preview_probe: what the pre-0.72.0 draw-then-undo pattern costs\n");
    std::printf("  %d stamps, voxel %.3f, brick dim %d, drag radius %.2f, %d events x %d repeats\n\n",
                kStamps, static_cast<double>(kVoxel), kBrickDim,
                static_cast<double>(kDragRadius), kEvents, kRepeats);

    std::vector<double> a_ms, b_ms;
    long a_bricks_total = 0, b_bricks_total = 0;
    int failures = 0;

    // The drag path: an arc across the form, anchored at its first point.
    const float anchor[3] = {0.0f, 0.6f, 0.35f};
    auto point_at = [&](int event) {
        const float t = static_cast<float>(event) / static_cast<float>(kEvents - 1);
        return std::array<float, 3>{anchor[0] + t * 0.45f, anchor[1] + t * 0.20f,
                                    anchor[2] + t * 0.10f};
    };

    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        // Alternate which arm runs first, so neither is always the colder one.
        const bool a_first = (repeat % 2) == 0;
        for (int which = 0; which < 2; ++which) {
            const bool arm_a = (which == 0) == a_first;

            Doc d;
            if (!build(&d)) {
                std::printf("FAIL: could not build the fixture\n");
                return 1;
            }
            clay_document_enable_undo(d.doc);
            clay_brick_cache* cache = make_cache();
            if (!cache) {
                std::printf("FAIL: could not create the brick cache\n");
                clay_document_destroy(d.doc);
                return 1;
            }

            // Warm: refill the whole form once, untimed, so neither arm pays
            // first-touch costs inside its measurement.
            const float warm_min[3] = {-2.0f, -1.5f, -1.0f};
            const float warm_max[3] = {2.0f, 1.5f, 1.0f};
            clay_brick_cache_mark_dirty(cache, warm_min, warm_max);
            if (drain(cache, d.doc) < 0) {
                std::printf("FAIL: warm refill failed\n");
                return 1;
            }

            clay_move_params mp;
            std::memset(&mp, 0, sizeof mp);
            mp.struct_size = sizeof mp;
            mp.radius = kDragRadius;
            mp.ease = 0;
            mp.front_only = 0;

            clay_sdf_move_tx* tx = clay_sdf_move_begin(d.doc, d.layer, anchor, &mp, nullptr);
            if (!tx) {
                std::printf("FAIL: clay_sdf_move_begin returned NULL\n");
                return 1;
            }
            std::size_t node_count = 0;
            clay_sdf_move_preview_nodes(tx, nullptr, 0, &node_count);
            std::vector<clay_node_id> nodes(node_count);
            if (node_count)
                clay_sdf_move_preview_nodes(tx, nodes.data(), nodes.size(), &node_count);

            std::vector<Grab> grabs;
            for (int event = 0; event < kEvents; ++event) {
                const auto p = point_at(event);
                const float total[3] = {p[0] - anchor[0], p[1] - anchor[1], p[2] - anchor[2]};

                clay_sculpt_dirty dirty;
                std::memset(&dirty, 0, sizeof dirty);
                dirty.struct_size = sizeof dirty;

                if (arm_a) {
                    // Untimed: resolve this frame's grabs. Arm A is not charged
                    // for the update that produces them -- see the header.
                    if (clay_sdf_move_update(tx, total, &dirty) != CLAY_OK) {
                        std::printf("FAIL: update failed on arm A\n");
                        return 1;
                    }
                    if (!read_grabs(tx, nodes, &grabs)) {
                        std::printf("FAIL: could not read the resolved grabs\n");
                        return 1;
                    }
                    if (!dirty.has_bounds) continue;

                    const Clock::time_point t0 = Clock::now();
                    clay_document_begin_undo_group(d.doc);
                    for (const Grab& g : grabs) {
                        // EIGHT params: centre(3), radius, displacement(3),
                        // front_only. Seven silently authored nothing, and
                        // because the return went unchecked the probe timed an
                        // arm that was not doing the work -- the defect its own
                        // field check caught.
                        const float params[8] = {g.centre[0],       g.centre[1],
                                                 g.centre[2],       g.radius,
                                                 g.displacement[0], g.displacement[1],
                                                 g.displacement[2],
                                                 static_cast<float>(g.front_only)};
                        if (clay_layer_add_deformer(d.doc, d.layer, g.node, CLAY_DEFORM_GRAB,
                                                    params, 8, g.ease,
                                                    /*at_front=*/1) != CLAY_OK) {
                            std::printf("FAIL: arm A could not author a grab\n");
                            return 1;
                        }
                    }
                    clay_document_end_undo_group(d.doc);
                    clay_brick_cache_mark_dirty(cache, dirty.bounds_min, dirty.bounds_max);
                    const long n = drain(cache, d.doc);
                    int32_t undone = 0;
                    clay_document_undo(d.doc, &undone);
                    a_ms.push_back(ms_since(t0));
                    if (n < 0) {
                        std::printf("FAIL: arm A refill failed\n");
                        return 1;
                    }
                    a_bricks_total += n;
                } else {
                    const Clock::time_point t0 = Clock::now();
                    if (clay_sdf_move_update(tx, total, &dirty) != CLAY_OK) {
                        std::printf("FAIL: update failed on arm B\n");
                        return 1;
                    }
                    if (!dirty.has_bounds) {
                        (void)ms_since(t0);
                        continue;
                    }
                    const clay_document* preview = clay_sdf_move_preview_document(tx);
                    if (!preview) {
                        std::printf("FAIL: preview document is NULL\n");
                        return 1;
                    }
                    clay_brick_cache_mark_dirty(cache, dirty.bounds_min, dirty.bounds_max);
                    const long n = drain(cache, preview);
                    b_ms.push_back(ms_since(t0));
                    if (n < 0) {
                        std::printf("FAIL: arm B refill failed\n");
                        return 1;
                    }
                    b_bricks_total += n;
                }
            }

            clay_sdf_move_cancel(tx);
            clay_sdf_move_destroy(tx);
            clay_brick_cache_destroy(cache);
            clay_document_destroy(d.doc);
        }
    }

    // -- the invariants -----------------------------------------------------
    //
    // Equal refill, or the timing compares two different amounts of work.
    std::printf("bricks refilled   A %ld   B %ld\n", a_bricks_total, b_bricks_total);
    if (a_bricks_total != b_bricks_total) {
        std::printf("FAIL: the two arms did not refill the same number of bricks, so the\n"
                    "      timing below would be region size rather than pattern. Fix the\n"
                    "      fixture before reading any ratio from it.\n");
        ++failures;
    }
    if (a_bricks_total == 0) {
        std::printf("FAIL: zero bricks refilled -- this probe measured nothing. A drag that\n"
                    "      reaches no surface times the empty path and reports it as a win.\n");
        ++failures;
    }
    if (a_ms.size() != b_ms.size() || a_ms.empty()) {
        std::printf("FAIL: sample counts differ (A %zu, B %zu)\n", a_ms.size(), b_ms.size());
        ++failures;
    }

    // Both arms must draw the same field, and the check that says so must be
    // one that could have said otherwise. Calibrate first, then assert.
    const auto last = point_at(kEvents - 1);
    const float final_total[3] = {last[0] - anchor[0], last[1] - anchor[1], last[2] - anchor[2]};

    // Calibrate: the same comparison on a drag that really is wrong by 0.15
    // world units. It MUST report a disagreement, or agreement below is not
    // evidence of anything.
    const float wrong_total[3] = {final_total[0] + 0.15f, final_total[1], final_total[2]};
    const double wrong = field_disagreement(wrong_total, final_total);
    std::printf("\ncalibration: preview at a drag 0.15 wrong disagrees by %.6f\n", wrong);
    if (wrong <= 1e-4) {
        std::printf("FAIL: the field check cannot distinguish a drag that is wrong by 0.15\n"
                    "      world units, so agreeing on the real one proves nothing. The\n"
                    "      probes are missing the warped region -- fix them before trusting\n"
                    "      any agreement below.\n");
        ++failures;
    }

    const double worst = field_disagreement(final_total, final_total);
    if (worst < 0.0) {
        std::printf("FAIL: could not compare the two arms' fields\n");
        ++failures;
    } else {
        std::printf("field disagreement A vs B: %.9f (max over 729 probes)\n", worst);
        if (worst > 1e-5) {
            std::printf("FAIL: the two arms drew DIFFERENT fields, so the timing above is not\n"
                        "      a comparison of two ways to do one thing.\n");
            ++failures;
        }
    }

    const double a50 = percentile(a_ms, 0.50), a95 = percentile(a_ms, 0.95);
    const double b50 = percentile(b_ms, 0.50), b95 = percentile(b_ms, 0.95);
    std::printf("\n  per pointer event      p50 ms     p95 ms   samples\n");
    std::printf("  A draw-then-undo      %8.4f   %8.4f   %7zu\n", a50, a95, a_ms.size());
    std::printf("  B preview document    %8.4f   %8.4f   %7zu\n", b50, b95, b_ms.size());
    if (b50 > 0.0) std::printf("\n  A / B  p50 %.2fx   p95 %.2fx\n", a50 / b50, a95 / b95);

    // The 0.125 ms derived regression floor: a per-event cost under it cannot
    // support a ratio claim, however clean the ratio looks.
    if (a50 < 0.125 || b50 < 0.125) {
        std::printf("\n  NOTE: at least one arm's p50 is under the 0.125 ms derived floor, so\n"
                    "  the ratio above is not a gateable number. Grow kStamps or kDragRadius\n"
                    "  until both arms clear it before quoting it anywhere.\n");
    }

    if (failures) {
        std::printf("\n%d invariant(s) failed.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
