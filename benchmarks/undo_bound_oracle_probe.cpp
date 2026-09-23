// The never-tighter property of the undo bound, re-derived on random
// documents (issue #639). Drives the C ABI only, so one source builds against
// any revision for an A/B.
//
// Each trial builds a document -- a base layer composed through a smooth or
// subtracting fold, a mirrored or radial layer, nested blended groups, items
// with blends, subtracts, rotations and (with "rich") intersects, per-axis
// scales, rounding, repeats, a moved layer and an instancing layer -- puts a
// chain of grab / magnify / blob links on one node, sometimes ahead of a
// twist, and then takes ONE step: a link added at the front, a link removed,
// or a host Move segment (clay_layer_move_surface). It undoes the step and
// redoes it, and for each direction refills ONLY the reported bound and
// compares every brick's state and fp16 payload with a cache rebuilt from
// nothing on a saved COPY of the document (a same-document rebuild resumes
// from the seeds a too-narrow bound failed to drop, and agrees with it).
//
//   undo_bound_oracle_probe FIRST COUNT [rich]
//
// Environment switches:
//   RV_RAWBOUND  also check the bound against the RAW field (clay_eval_points):
//                every sample whose value moved within the band must lie in
//                the bound dilated by the band. This separates a bound that is
//                too tight from a full rebuild that disagrees with the field.
//   RV_RAWCHECK  instead compare each document's full brick build with its raw
//                field at in-band samples (a property of the engine, not of
//                the undo bound).
//   RV_FORWARD   instead check the FORWARD Move: dirty exactly the regions
//                clay_layer_move_surface_regions reports, compare with a rebuild.
//   RV_VERBOSE   trace the fixture and every stale brick.
//
// Measured with it, see openspec/changes/archive/2026-09-23-bound-an-undone-grab-by-its-support/
// tasks.md ("What review found").
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "clay.h"

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr float kWorld = 2.4f;
bool g_rich = false;
bool g_verbose = false;
bool g_rawbound = false;
size_t g_raw_violations = 0, g_raw_violation_trials = 0;
#define V(...) do { if (g_verbose) std::printf(__VA_ARGS__); } while (0)

#define OK(x)                                                                  \
    do {                                                                       \
        clay_result r_ = (x);                                                  \
        if (r_ != CLAY_OK) {                                                   \
            std::fprintf(stderr, "FAIL %s -> %d line %d\n", #x, (int)r_, __LINE__); \
            std::exit(2);                                                      \
        }                                                                      \
    } while (0)

struct Box {
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int32_t has = 0, inf = 0;
};

clay_brick_cache* make_cache() {
    clay_brick_config cfg;
    cfg.struct_size = sizeof cfg;
    OK(clay_brick_config_defaults(&cfg));
    cfg.dim = kDim;
    cfg.voxel_size = kVoxel;
    cfg.band_voxels = 3;
    cfg.memory_budget = 0;
    return clay_brick_cache_create(&cfg);
}

size_t refill(clay_brick_cache* c, const clay_document* d) {
    const size_t chunk = 64;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> vals(chunk * kSamples);
    std::vector<int32_t> res(chunk);
    size_t n = 0;
    for (;;) {
        size_t count = chunk, rem = 0;
        OK(clay_brick_cache_take_dirty(c, reqs.data(), &count, &rem));
        if (!count) break;
        OK(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), count, vals.data(),
                                          count * kSamples, nullptr, 0));
        size_t acc = 0;
        OK(clay_brick_cache_submit(c, reqs.data(), count, vals.data(), count * kSamples, nullptr,
                                   0, res.data(), &acc));
        n += count;
        if (!rem) break;
    }
    return n;
}

std::vector<int32_t> world_keys() {
    const float b = kVoxel * kDim;
    const int lo = (int)std::floor(-kWorld / b), hi = (int)std::floor(kWorld / b);
    std::vector<int32_t> k;
    for (int z = lo; z <= hi; ++z)
        for (int y = lo; y <= hi; ++y)
            for (int x = lo; x <= hi; ++x) {
                k.push_back(x);
                k.push_back(y);
                k.push_back(z);
            }
    return k;
}

struct Snap {
    std::vector<int32_t> st;
    std::vector<uint16_t> h;
};

Snap snap(clay_brick_cache* c, const std::vector<int32_t>& keys) {
    size_t n = keys.size() / 3;
    Snap s;
    s.st.assign(n, -1);
    s.h.assign(n * kSamples, 0);
    OK(clay_brick_cache_read_bricks(c, 0, keys.data(), n, 0, s.st.data(), s.h.data(),
                                    n * kSamples, nullptr, 0));
    return s;
}

size_t diff(const Snap& a, const Snap& b) {
    size_t d = 0;
    for (size_t i = 0; i < a.st.size(); ++i)
        if (a.st[i] != b.st[i] ||
            std::memcmp(&a.h[i * kSamples], &b.h[i * kSamples], kSamples * 2) != 0)
            ++d;
    return d;
}

void mark_world(clay_brick_cache* c) {
    const float lo[3] = {-kWorld, -kWorld, -kWorld}, hi[3] = {kWorld, kWorld, kWorld};
    OK(clay_brick_cache_mark_dirty(c, lo, hi));
}

Snap rebuilt(clay_document* d, const std::vector<int32_t>& keys) {
    clay_blob* blob = nullptr;
    OK(clay_document_save_memory(d, &blob));
    clay_document* copy = nullptr;
    OK(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &copy));
    clay_blob_destroy(blob);
    clay_brick_cache* c = make_cache();
    mark_world(c);
    refill(c, copy);
    Snap s = snap(c, keys);
    clay_brick_cache_destroy(c);
    clay_document_destroy(copy);
    return s;
}

struct Rng {
    std::mt19937 g;
    explicit Rng(unsigned s) : g(s) {}
    float u(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); }
    int i(int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); }
    bool p(float q) { return u(0, 1) < q; }
};

clay_node_id add_item(clay_document* d, clay_layer_id l, Rng& r, clay_node_id group, bool in_group) {
    clay_item* it = nullptr;
    if (r.p(0.7f)) {
        float rad = r.u(0.25f, 0.8f);
        it = clay_item_create(CLAY_PRIM_SPHERE, &rad, 1);
    } else {
        float bx[3] = {r.u(0.2f, 0.6f), r.u(0.2f, 0.6f), r.u(0.2f, 0.6f)};
        it = clay_item_create(CLAY_PRIM_BOX, bx, 3);
    }
    float pos[3] = {r.u(-0.9f, 0.9f), r.u(-0.9f, 0.9f), r.u(-0.9f, 0.9f)};
    OK(clay_item_set_position(it, pos));
    V("item pos %.3f %.3f %.3f in_group=%d group=%u\n", pos[0], pos[1], pos[2], (int)in_group, (unsigned)group);
    if (r.p(0.4f)) {
        float ax[3] = {r.u(-1, 1), r.u(-1, 1), r.u(0.1f, 1)};
        OK(clay_item_set_rotation(it, ax, r.u(-2.0f, 2.0f)));
    }
    if (r.p(0.3f)) OK(clay_item_set_scale(it, r.u(0.7f, 1.3f)));
    if (r.p(0.6f)) { float k = r.u(0.05f, 0.3f); V("  blend %.3f\n", k); OK(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, k)); }
    if (r.p(0.15f)) { V("  subtract\n"); OK(clay_item_set_op(it, CLAY_OP_SUBTRACT)); }
    if (g_rich) {
        if (r.p(0.3f)) {
            float sc[3] = {r.u(0.6f, 1.4f), r.u(0.6f, 1.4f), r.u(0.6f, 1.4f)};
            OK(clay_item_set_scale_nonuniform(it, sc));
        }
        if (r.p(0.3f)) OK(clay_item_set_rounding(it, r.u(0.0f, 0.08f)));
        if (r.p(0.1f)) OK(clay_item_set_op(it, CLAY_OP_INTERSECT));
        if (r.p(0.15f)) OK(clay_item_set_repeat_radial(it, r.i(2, 4), r.u(0.2f, 0.6f)));
        else if (r.p(0.1f)) {
            const float s1 = r.u(0.6f, 1.0f); float sp[3] = {s1, s1, s1};
            float cn[3] = {1, 0, 1};
            OK(clay_item_set_repeat_grid(it, sp, cn));
        }
        if (r.p(0.15f)) OK(clay_item_set_mirror(it, 0));
    }
    clay_node_id n = 0;
    if (in_group)
        OK(clay_layer_add_item_in_group(d, l, group, -1, it, &n));
    else
        OK(clay_layer_add_item(d, l, it, &n));
    clay_item_destroy(it);
    return n;
}

// A random finite-support link at the front; params in the node's frame.
void add_head_link(clay_document* d, clay_layer_id l, clay_node_id n, Rng& r, float kind_bias) {
    const float c[3] = {r.u(-0.9f, 0.9f), r.u(-0.9f, 0.9f), r.u(-0.9f, 0.9f)};
    const float rad = r.u(0.08f, 0.5f);
    const float k = r.u(0, 1);
    const int32_t ease = r.p(0.2f) ? 1 : 0;
    V("link node=%u c=%.3f %.3f %.3f r=%.3f kind=%.2f<%.2f ease=%d\n", (unsigned)n, c[0], c[1], c[2], rad, k, kind_bias, ease);
    if (k < kind_bias) {
        float pr[8] = {c[0], c[1], c[2], rad, r.u(-0.4f, 0.4f), r.u(-0.4f, 0.4f),
                       r.u(-0.4f, 0.4f), r.p(0.3f) ? 1.0f : 0.0f};
        V("  grab pull %.3f %.3f %.3f front %.0f\n", pr[4], pr[5], pr[6], pr[7]);
        OK(clay_layer_add_deformer(d, l, n, CLAY_DEFORM_GRAB, pr, 8, ease, 1));
    } else if (k < (1 + kind_bias) / 2) {
        float pr[5] = {c[0], c[1], c[2], rad, r.u(-0.6f, 0.6f)};
        OK(clay_layer_add_deformer(d, l, n, CLAY_DEFORM_MAGNIFY, pr, 5, ease, 1));
    } else {
        float pr[9] = {c[0], c[1], c[2], rad, r.u(0.02f, 0.1f), 6.0f, 3.0f, 0.5f, 7.0f};
        OK(clay_layer_add_deformer(d, l, n, CLAY_DEFORM_BLOB, pr, 9, ease, 1));
    }
}

struct Trial {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    clay_node_id target = 0;
};

Trial build(unsigned seed, Rng& r) {
    (void)seed;
    Trial t;
    t.d = clay_document_create();
    OK(clay_add_sdf_layer(t.d, "body", &t.layer));
    OK(clay_document_enable_undo(t.d));
    if (r.p(0.5f)) {  // a base layer under, composed through a fold
        clay_layer_id base = 0;
        OK(clay_add_sdf_layer(t.d, "base", &base));
        OK(clay_document_move_layer(t.d, base, 0));
        const int32_t op = r.p(0.7f) ? CLAY_OP_ADD : CLAY_OP_SUBTRACT;
        V("BASE LAYER op=%d (items below are base until 'body')\n", op);
        OK(clay_document_set_layer_composition(t.d, t.layer, op, CLAY_BLEND_QUADRATIC,
                                               r.u(0.05f, 0.3f), 0.0f));
        for (int i = 0, n = r.i(1, 2); i < n; ++i) add_item(t.d, base, r, 0, false);
    }
    V("body\n");
    if (r.p(0.4f)) {
        const int mx = r.p(0.7f), my = r.p(0.3f), mz = r.p(0.3f);
        const float mk = r.u(0.0f, 0.2f);
        V("mirror %d %d %d k=%.3f\n", mx, my, mz, mk);
        OK(clay_set_layer_mirror(t.d, t.layer, mx, my, mz, mk));
    } else if (r.p(0.2f)) {
        const int ax = r.i(0, 2), cnt = r.i(2, 5);
        const float rk = r.u(0.0f, 0.2f);
        V("radial axis %d count %d k=%.3f\n", ax, cnt, rk);
        OK(clay_set_layer_radial(t.d, t.layer, ax, cnt, rk));
    }
    if (g_rich && r.p(0.35f)) {
        const float pos[3] = {r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f)};
        const float ax[3] = {r.u(-1, 1), r.u(0.1f, 1), r.u(-1, 1)};
        const float sc[3] = {r.u(0.7f, 1.3f), r.u(0.7f, 1.3f), r.u(0.7f, 1.3f)};
        OK(clay_document_set_layer_transform_nonuniform(t.d, t.layer, pos, ax, r.u(-1.5f, 1.5f), sc));
    }
    clay_node_id group = 0;
    const bool use_group = r.p(0.5f);
    if (use_group) {
        clay_node_id parent = 0;
        if (r.p(0.4f)) {  // nested group
            OK(clay_layer_add_group(t.d, t.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                    r.u(0.05f, 0.3f), 0.0f, &parent));
            add_item(t.d, t.layer, r, parent, true);
        }
        OK(clay_layer_add_group(t.d, t.layer, parent, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                r.u(0.05f, 0.3f), r.p(0.3f) ? r.u(0.0f, 0.05f) : 0.0f, &group));
    }
    std::vector<clay_node_id> nodes;
    for (int i = 0, n = r.i(2, 4); i < n; ++i)
        nodes.push_back(add_item(t.d, t.layer, r, group, use_group && r.p(0.7f)));
    t.target = nodes[r.i(0, (int)nodes.size() - 1)];
    V("target %u\n", (unsigned)t.target);
    // A tail: sometimes a twist behind everything, so head links sit ahead of it.
    if (r.p(0.25f)) {
        float k = r.u(-1.0f, 1.0f);
        OK(clay_layer_add_deformer(t.d, t.layer, t.target, CLAY_DEFORM_TWIST, &k, 1, 0, 0));
    }
    for (int i = 0, n = r.i(0, 8); i < n; ++i) add_head_link(t.d, t.layer, t.target, r, 0.7f);
    if (g_rich && r.p(0.3f)) {
        clay_layer_id inst = 0;
        OK(clay_document_instance_layer(t.d, t.layer, "inst", &inst));
        const float pos[3] = {r.u(-0.8f, 0.8f), r.u(-0.8f, 0.8f), r.u(-0.8f, 0.8f)};
        const float ax[3] = {0, 1, 0};
        OK(clay_document_set_layer_transform(t.d, inst, pos, ax, r.u(-1.5f, 1.5f), r.u(0.7f, 1.2f)));
    }
    return t;
}

// The step under test, recorded on the undo stack.
int do_step(Trial& t, Rng& r) {
    const int kind = r.i(0, 3);
    if (kind == 0 || kind == 1) {
        add_head_link(t.d, t.layer, t.target, r, 0.8f);
        return 0;
    }
    if (kind == 2) {  // remove a link at a random index (0 is the head)
        add_head_link(t.d, t.layer, t.target, r, 0.8f);
        if (clay_layer_remove_deformer(t.d, t.layer, t.target, (size_t)r.i(0, 1)) != CLAY_OK)
            OK(clay_layer_remove_deformer(t.d, t.layer, t.target, 0));
        return 2;
    }
    // A host Move segment through the surface-level gesture.
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.radius = r.u(0.1f, 0.4f);
    const float c[3] = {r.u(-1, 1), r.u(-1, 1), r.u(-1, 1)};
    const float pull[3] = {r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f)};
    size_t applied = 0;
    const clay_result mr = clay_layer_move_surface(t.d, t.layer, c, pull, &p, &applied);
    V("move c=%.3f %.3f %.3f pull %.3f %.3f %.3f r=%.3f applied=%zu rc=%d\n", c[0], c[1], c[2], pull[0], pull[1], pull[2], p.radius, applied, (int)mr);
    if (mr != CLAY_OK || applied == 0) {
        add_head_link(t.d, t.layer, t.target, r, 1.0f);
        return 0;
    }
    return 3;
}

Box node_bound(Trial& t) {
    Box b;
    OK(clay_layer_node_influence_bound(t.d, t.layer, t.target, b.lo, b.hi, &b.has, &b.inf));
    return b;
}

bool contains(const Box& outer, const Box& inner) {
    if (!inner.has) return true;
    if (!outer.has) return false;
    if (outer.inf) return true;
    if (inner.inf) return false;
    for (int a = 0; a < 3; ++a)
        if (inner.lo[a] < outer.lo[a] - 1e-6f || inner.hi[a] > outer.hi[a] + 1e-6f) return false;
    return true;
}

Box join(const Box& a, const Box& b) {
    if (!a.has) return b;
    if (!b.has) return a;
    Box o = a;
    o.inf = a.inf || b.inf;
    for (int i = 0; i < 3; ++i) {
        o.lo[i] = std::fmin(a.lo[i], b.lo[i]);
        o.hi[i] = std::fmax(a.hi[i], b.hi[i]);
    }
    return o;
}

struct Stats {
    size_t trials = 0, stale_trials = 0, larger = 0, visible = 0, narrowed = 0;
    size_t refilled = 0, node_refill = 0;
};

// One direction: returns stale bricks.
std::vector<float> brick_points(const std::vector<int32_t>& keys) {
    std::vector<float> pts;
    const float b = kVoxel * kDim;
    for (size_t i = 0; i < keys.size() / 3; ++i)
        for (int z = 0; z < kDim; ++z)
            for (int y = 0; y < kDim; ++y)
                for (int x = 0; x < kDim; ++x) {
                    pts.push_back(keys[3 * i] * b + x * kVoxel);
                    pts.push_back(keys[3 * i + 1] * b + y * kVoxel);
                    pts.push_back(keys[3 * i + 2] * b + z * kVoxel);
                }
    return pts;
}

std::vector<float> eval_all(clay_document* d, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    OK(clay_eval_points(d, nullptr, pts.data(), out.size(), out.data(), nullptr));
    return out;
}

float half_to_float(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : std::ldexp(1.0f + m / 1024.0f, e - 15);
    return (h & 0x8000) ? -v : v;
}

size_t check_dir(Trial& t, bool undo, const std::vector<int32_t>& keys, Stats& s, unsigned seed,
                 int kind) {
    clay_brick_cache* kept = make_cache();
    mark_world(kept);
    refill(kept, t.d);
    const Snap before = snap(kept, keys);
    std::vector<float> pts, raw_before;
    if (g_verbose || g_rawbound) { pts = brick_points(keys); raw_before = eval_all(t.d, pts); }
    const Box nb_before = node_bound(t);
    Box b;
    int32_t done = 0;
    if (undo)
        OK(clay_document_undo_bound(t.d, &done, b.lo, b.hi, &b.has, &b.inf));
    else
        OK(clay_document_redo_bound(t.d, &done, b.lo, b.hi, &b.has, &b.inf));
    const Box nb_after = node_bound(t);
    V("node bound before [%.3f %.3f %.3f]-[%.3f %.3f %.3f] has=%d inf=%d\n", nb_before.lo[0], nb_before.lo[1], nb_before.lo[2], nb_before.hi[0], nb_before.hi[1], nb_before.hi[2], nb_before.has, nb_before.inf);
    V("node bound after  [%.3f %.3f %.3f]-[%.3f %.3f %.3f] has=%d inf=%d\n", nb_after.lo[0], nb_after.lo[1], nb_after.lo[2], nb_after.hi[0], nb_after.hi[1], nb_after.hi[2], nb_after.has, nb_after.inf);
    // Node-bound-of-target (what main reported for a pure single-node step).
    const Box nb = join(nb_before, nb_after);
    if (kind != 3 && !contains(nb, b)) {
        ++s.larger;
        std::printf("seed %u %s: bound LARGER than node bound\n", seed, undo ? "undo" : "redo");
    }
    if (g_rawbound) {
        // THE DIRECT CHECK: every sample whose raw value moved, and is within
        // the band on either side, lies inside the reported bound dilated by
        // the band (what mark_dirty adds).
        const std::vector<float> raw_after = eval_all(t.d, pts);
        const float band = 0.15f, eps = 1e-4f;
        size_t v = 0;
        for (size_t i = 0; i < raw_after.size(); ++i) {
            if (raw_after[i] == raw_before[i]) continue;
            if (std::fabs(raw_after[i]) >= band && std::fabs(raw_before[i]) >= band) continue;
            bool inside = b.has && (b.inf || true);
            if (b.has && !b.inf)
                for (int a = 0; a < 3; ++a)
                    if (pts[3 * i + a] < b.lo[a] - band - eps || pts[3 * i + a] > b.hi[a] + band + eps) inside = false;
            if (!inside) {
                if (v < 3) std::printf("seed %u %s: raw moved OUTSIDE bound+band at %.3f %.3f %.3f (%.5f -> %.5f)\n", seed, undo ? "undo" : "redo", pts[3*i], pts[3*i+1], pts[3*i+2], raw_before[i], raw_after[i]);
                ++v;
            }
        }
        g_raw_violations += v;
        if (v) ++g_raw_violation_trials;
    }
    if (b.has && !b.inf) OK(clay_brick_cache_mark_dirty(kept, b.lo, b.hi));
    if (b.has && b.inf) mark_world(kept);
    s.refilled += refill(kept, t.d);
    const Snap got = snap(kept, keys);
    const Snap want = rebuilt(t.d, keys);
    const size_t stale = diff(got, want);
    if (g_verbose && stale) {
        const std::vector<float> raw_after = eval_all(t.d, pts);
        size_t moved = 0, moved_band = 0; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t i = 0; i < raw_after.size(); ++i)
            if (raw_after[i] != raw_before[i]) {
                ++moved;
                if (std::fabs(raw_after[i]) < 0.16f || std::fabs(raw_before[i]) < 0.16f) {
                    ++moved_band;
                    for (int a = 0; a < 3; ++a) { lo[a] = std::fmin(lo[a], pts[3*i+a]); hi[a] = std::fmax(hi[a], pts[3*i+a]); }
                }
            }
        V("  raw field moved at %zu samples, %zu in band, band-moved box [%.3f %.3f %.3f]-[%.3f %.3f %.3f]\n", moved, moved_band, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    }
    if (g_verbose)
        for (size_t i = 0; i < got.st.size(); ++i)
            if (got.st[i] != want.st[i] || std::memcmp(&got.h[i * kSamples], &want.h[i * kSamples], kSamples * 2) != 0) {
                int maxd = 0; float gv = 0, wv = 0; size_t mj = 0;
                for (size_t j = 0; j < kSamples; ++j) { int d = std::abs((int)got.h[i*kSamples+j] - (int)want.h[i*kSamples+j]); if (d > maxd) { maxd = d; gv = half_to_float(got.h[i*kSamples+j]); wv = half_to_float(want.h[i*kSamples+j]); mj = j; } }
                if (!raw_before.empty()) {
                    const std::vector<float> ra = eval_all(t.d, std::vector<float>(pts.begin() + 3 * (i * kSamples + mj), pts.begin() + 3 * (i * kSamples + mj) + 3));
                    V("    sample %zu: kept %.5f rebuilt %.5f raw_before %.5f raw_after %.5f\n", mj, gv, wv, raw_before[i * kSamples + mj], ra[0]);
                }
                V("  stale key %d %d %d state %d vs %d max half-diff %d (%g vs %g raw)\n", keys[3*i], keys[3*i+1], keys[3*i+2], got.st[i], want.st[i], maxd, gv, wv);
            }
    if (diff(before, want) > 0) ++s.visible;
    if (kind != 3 && contains(nb, b) && !(contains(b, nb))) ++s.narrowed;
    if (stale)
        std::printf("seed %u kind %d %s: STALE %zu bricks  bound=[%.3f %.3f %.3f]-[%.3f %.3f %.3f] has=%d inf=%d\n",
                    seed, kind, undo ? "undo" : "redo", stale, b.lo[0], b.lo[1], b.lo[2], b.hi[0],
                    b.hi[1], b.hi[2], b.has, b.inf);
    clay_brick_cache_destroy(kept);
    return stale;
}

}  // namespace

// The FORWARD Move's own oracle: build the trial, and instead of the random
// step do the kind-3 move with clay_layer_move_surface_regions, dirty exactly
// the regions it reports, and compare with a rebuild.
size_t forward_move_check(unsigned seed, const std::vector<int32_t>& keys) {
    Rng r(seed);
    Trial t = build(seed, r);
    const int kind = r.i(0, 3);
    if (kind != 3) { clay_document_destroy(t.d); return 0; }
    clay_brick_cache* kept = make_cache();
    mark_world(kept);
    refill(kept, t.d);
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.radius = r.u(0.1f, 0.4f);
    const float c[3] = {r.u(-1, 1), r.u(-1, 1), r.u(-1, 1)};
    const float pull[3] = {r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f), r.u(-0.3f, 0.3f)};
    size_t applied = 0, nbox = 0;
    std::vector<float> boxes(6 * 64);
    if (clay_layer_move_surface_regions(t.d, t.layer, c, pull, &p, &applied, boxes.data(), 64, &nbox) != CLAY_OK || applied == 0) {
        clay_brick_cache_destroy(kept); clay_document_destroy(t.d); return 0;
    }
    for (size_t i = 0; i < nbox; ++i) OK(clay_brick_cache_mark_dirty(kept, &boxes[6 * i], &boxes[6 * i + 3]));
    refill(kept, t.d);
    const size_t stale = diff(snap(kept, keys), rebuilt(t.d, keys));
    std::printf("forward seed %u applied %zu boxes %zu: stale %zu\n", seed, applied, nbox, stale);
    clay_brick_cache_destroy(kept);
    clay_document_destroy(t.d);
    return stale;
}

// A document's full brick build against its raw field, at in-band samples.
float build_vs_raw(clay_document* d, const std::vector<int32_t>& keys, size_t* bad) {
    clay_brick_cache* c = make_cache();
    mark_world(c);
    refill(c, d);
    const Snap s = snap(c, keys);
    const std::vector<float> pts = brick_points(keys);
    const std::vector<float> raw = eval_all(d, pts);
    float worst = 0;
    *bad = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (s.st[i / kSamples] != 2 || std::fabs(raw[i]) > 0.12f) continue;
        const float e = std::fabs(half_to_float(s.h[i]) - raw[i]);
        if (e > 2e-3f) ++*bad;
        worst = std::fmax(worst, e);
    }
    clay_brick_cache_destroy(c);
    return worst;
}

int main(int argc, char** argv) {
    if (std::getenv("RV_RAWCHECK")) {
        g_rich = argc > 3 && argv[3][0] == 'r';
        const std::vector<int32_t> keys = world_keys();
        const unsigned first = (unsigned)std::atoi(argv[1]), count = (unsigned)std::atoi(argv[2]);
        size_t docs_bad = 0;
        for (unsigned seed = first; seed < first + count; ++seed) {
            Rng r(seed);
            Trial t = build(seed, r);
            size_t bad = 0;
            const float w = build_vs_raw(t.d, keys, &bad);
            if (bad) { ++docs_bad; std::printf("seed %u: %zu in-band samples off by > 2e-3, worst %.5f\n", seed, bad, w); }
            clay_document_destroy(t.d);
        }
        std::printf("raw check: %zu of %u documents\n", docs_bad, count);
        return 0;
    }
    if (std::getenv("RV_FORWARD")) {
        g_rich = argc > 3 && argv[3][0] == 'r';
        const std::vector<int32_t> keys = world_keys();
        const unsigned first = (unsigned)std::atoi(argv[1]), count = (unsigned)std::atoi(argv[2]);
        size_t bad = 0;
        for (unsigned s = first; s < first + count; ++s) bad += forward_move_check(s, keys) ? 1 : 0;
        std::printf("forward stale trials %zu\n", bad);
        return 0;
    }
    const unsigned first = argc > 1 ? (unsigned)std::atoi(argv[1]) : 1;
    const unsigned count = argc > 2 ? (unsigned)std::atoi(argv[2]) : 50;
    g_rich = argc > 3 && argv[3][0] == 'r';
    g_verbose = std::getenv("RV_VERBOSE") != nullptr;
    g_rawbound = std::getenv("RV_RAWBOUND") != nullptr;
    const std::vector<int32_t> keys = world_keys();
    Stats s;
    for (unsigned seed = first; seed < first + count; ++seed) {
        Rng r(seed);
        Trial t = build(seed, r);
        const int kind = do_step(t, r);
        const size_t su = check_dir(t, true, keys, s, seed, kind);
        const size_t sr = check_dir(t, false, keys, s, seed, kind);
        ++s.trials;
        if (su || sr) ++s.stale_trials;
        clay_document_destroy(t.d);
    }
    if (g_rawbound) std::printf("raw-bound violations: %zu samples in %zu directions\n", g_raw_violations, g_raw_violation_trials);
    std::printf("trials=%zu stale_trials=%zu larger_than_node=%zu edit_visible_dirs=%zu narrowed_dirs=%zu refilled=%zu\n",
                s.trials, s.stale_trials, s.larger, s.visible, s.narrowed, s.refilled);
    return s.stale_trials || s.larger ? 1 : 0;
}
