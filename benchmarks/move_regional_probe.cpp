// DOES THE MOVE ARCHITECTURE ALREADY EXIST, AND WAS IT SIMPLY NEVER INVOKED
// FOR MOVE? NOT a gated benchmark.
//
// THE QUESTION. `benchmarks/move_collapse_crossover_probe.cpp` compared a
// degraded Move chain against WHOLE-LAYER consolidation
// (`clay_layer_consolidate` with a NULL region) and found a crossover near a
// step scale of 0.15. Issue #534 proposed a threshold from that table. The
// table answers:
//
//     when does baking the ENTIRE LAYER beat the degraded chain?
//
// which is not the question a sculptor poses. They work a PATCH. The question
// is:
//
//     when does merging THIS PATCH amortise better than leaving it parametric?
//
// Those are different functions and there is no reason they cross in the same
// place. `clay_layer_consolidate_region` already exists and already claims the
// property the whole effort has been looking for:
//
//     "the second gesture on a patch has the first gesture's volume inside its
//      closure, so it is absorbed rather than stacked on. A patch stays at ONE
//      baked item however many times it is worked -- O(1) in gestures where
//      appending was O(n)."
//
// Nobody has ever pointed it at a Move chain. This does.
//
// THE FIVE ARMS, and D and E are the point:
//
//   A  raw Move chain
//   B  whole-layer consolidation            <- all #534 measured
//   C  ONE regional consolidation at the end
//   D  regional consolidation, then another Move on top
//   E  REPEATED regional maintenance across the whole session
//
// A, B and C ask whether one bake helps one degraded chain. **E asks whether
// interactive cost stays BOUNDED while the artist keeps working the same
// patch**, which is the property the engine actually needs and the only arm
// that can demonstrate it.
//
// THE TRAP THIS IS BUILT TO CATCH. The region is not the brush box: what gets
// sampled is the INFLUENCE CLOSURE, grown until every item that can reach
// inside it is wholly inside it. That is what makes the bake sound -- absorb
// only the overlapping items and a Subtract straddling the edge stays behind,
// the material it carved comes back, and the volume cannot remove it again.
// But a closure can SWALLOW THE LAYER: one item reaching everywhere pulls in
// all the rest, `whole_layer` goes true, and regional consolidation IS
// whole-layer consolidation, buying nothing.
//
// A plain sphere is the BEST CASE for closure locality, so measuring only that
// would prove nothing about a real form. Hence the topology sweep, and hence
// `closure / requested` being reported for every row -- it may be the single
// most important number here. Ratios near 1-2x mean this works. Routine
// `whole_layer`, or ratios in the tens, mean the mechanism exists and is not
// the practical cure.
//
// AND IT IS A CORRECTNESS QUESTION, NOT ONLY A LATENCY ONE. The crossover probe
// found that at 16 dabs sphere-tracing returns 0 hits of 1844 -- the field does
// not render slowly, it renders WRONG. So every arm reports its HIT RATE. If
// maintenance keeps the hit rate while the raw arm loses it, regional
// consolidation is a correctness-preserving mechanism rather than an
// optimisation, which is a far stronger argument for triggering it.
//
// Exits non-zero if it cannot discriminate: if the chain never grows, if a bake
// silently fails, or if the reference arm finds no surface to begin with.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

constexpr float kMoveRadius = 0.40f;
constexpr int kRaySide = 48;  // 2304 rays; enough to see a hit-rate collapse
constexpr float kCell = 0.02f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

// -- the scene topologies ---------------------------------------------------
//
// A plain sphere is the best case for closure locality and the worst case for
// learning anything. These vary what the closure has to swallow.
enum Topology {
    kSphere = 0,          // one primitive: the control
    kLocalSubtract,       // a small cutter away from the patch
    kCrossingSubtract,    // a LARGE cutter crossing the patch -- the closure risk
    kSmoothUnionForm,     // a multi-part blended form
    kManyDetails,         // twenty independent local items
    kTopologyCount,
};

const char* topology_name(int t) {
    switch (t) {
        case kSphere: return "sphere";
        case kLocalSubtract: return "sphere+local-subtract";
        case kCrossingSubtract: return "sphere+crossing-subtract";
        case kSmoothUnionForm: return "smooth-union form";
        case kManyDetails: return "sphere+20 details";
        default: return "?";
    }
}

bool add_sphere(Doc* d, float r, const float p[3], int op, float blend_k) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!it) return false;
    clay_item_set_position(it, p);
    clay_item_set_op(it, op);
    if (blend_k > 0.0f) clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, blend_k);
    clay_node_id n = 0;
    const clay_result res = clay_layer_add_item(d->doc, d->layer, it, &n);
    clay_item_destroy(it);
    return res == CLAY_OK;
}

bool build(Doc* out, int topology) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    Doc d{doc, layer};
    const float origin[3] = {0, 0, 0};
    bool ok = add_sphere(&d, 1.0f, origin, CLAY_OP_ADD, 0.0f);

    if (topology == kLocalSubtract) {
        const float p[3] = {0.0f, -0.85f, 0.0f};  // away from the patch (+z)
        ok = ok && add_sphere(&d, 0.30f, p, CLAY_OP_SUBTRACT, 0.0f);
    } else if (topology == kCrossingSubtract) {
        // Large, and it crosses where the Move patch will be. This is the case
        // that should pull the closure open.
        const float p[3] = {0.0f, 0.0f, 0.90f};
        ok = ok && add_sphere(&d, 0.80f, p, CLAY_OP_SUBTRACT, 0.0f);
    } else if (topology == kSmoothUnionForm) {
        for (int i = 0; i < 4 && ok; ++i) {
            const float a = static_cast<float>(i) * 1.5707963f;
            const float p[3] = {std::cos(a) * 0.7f, std::sin(a) * 0.7f, 0.0f};
            ok = add_sphere(&d, 0.5f, p, CLAY_OP_ADD, 0.08f);
        }
    } else if (topology == kManyDetails) {
        for (int i = 0; i < 20 && ok; ++i) {
            const float a = static_cast<float>(i) * 0.314159f;
            const float p[3] = {std::cos(a) * 0.95f, std::sin(a) * 0.95f,
                                (i % 2) ? 0.25f : -0.25f};
            ok = add_sphere(&d, 0.14f, p, CLAY_OP_ADD, 0.02f);
        }
    }
    if (!ok) {
        clay_document_destroy(doc);
        return false;
    }
    *out = d;
    return true;
}

// A box in world space: the region a host hands to a region merge. Every one
// used here is MEASURED -- the engine's own dirty bounds from
// clay_sdf_move_update -- rather than hand-written, which is the correction
// that made this probe worth running.
struct Patch {
    float min[3];
    float max[3];
};

double box_volume(const float lo[3], const float hi[3]) {
    const double x = std::max(0.0f, hi[0] - lo[0]);
    const double y = std::max(0.0f, hi[1] - lo[1]);
    const double z = std::max(0.0f, hi[2] - lo[2]);
    return x * y * z;
}

// One Move dab on the patch, at a fresh centre so it does not coalesce with the
// last (continues_gesture matches bit-exact centre and radius).
// `out_dirty`, when given, receives the ENGINE'S OWN conservative swept ball
// for the gesture -- where the surface was, united with where it went. That is
// region policy (1) below, and taking it from the engine rather than
// reconstructing it is the whole point: a host has this number already, from
// clay_sdf_move_update, and would pass exactly it.
bool dab(Doc* d, int index, Patch* out_dirty) {
    const float t = 0.37f * static_cast<float>(index);
    const float cx = std::cos(t) * 0.42f;
    const float cy = std::sin(t) * 0.42f;
    const float cz = std::sqrt(std::max(0.05f, 1.0f - cx * cx - cy * cy));
    const float anchor[3] = {cx, cy, cz};

    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kMoveRadius;
    mp.ease = 0;
    mp.front_only = 0;

    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, anchor, &mp, nullptr);
    if (!tx) return false;
    const float total[3] = {cx * 0.11f, cy * 0.11f, cz * 0.11f};
    clay_sculpt_dirty dirty;
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    bool ok = clay_sdf_move_update(tx, total, &dirty) == CLAY_OK;
    if (ok) {
        clay_sculpt_budget b;
        std::memset(&b, 0, sizeof b);
        b.struct_size = sizeof b;
        ok = clay_sdf_move_commit(tx, &b) == CLAY_OK;
    }
    if (ok && out_dirty && dirty.has_bounds) {
        for (int a = 0; a < 3; ++a) {
            out_dirty->min[a] = dirty.bounds_min[a];
            out_dirty->max[a] = dirty.bounds_max[a];
        }
    }
    clay_sdf_move_destroy(tx);
    return ok;
}

// -- WHICH BOX DOES A HOST PASS? --------------------------------------------
//
// The probe's own arms sweep topology and policy; the REGION is the host's
// choice and the result depends on it. Three plausible ones, and they are not
// equivalent:
//
//   1 SWEPT    the gesture's own dirty region, straight from
//              clay_sdf_move_update -- the tightest honest box, and what
//              refill_preview already computes
//   2 UNION    every dirty region since the last maintenance -- bigger box,
//              fewer bakes; what an at-pointer-up policy accumulates
//   3 ANCHOR   the brush footprint at the anchor, ignoring travel -- smallest,
//              and probably wrong for a long drag
//
// The difference between 1 and 2 IS the maintenance policy question: bake every
// gesture over a tight box, or bake rarely over a loose one. Reporting only one
// of them would answer for whichever the fixture happened to resemble, which is
// the trap this file exists to avoid elsewhere.
enum RegionPolicy { kSwept = 0, kUnion, kAnchor, kRegionPolicyCount };

const char* region_name(int r) {
    switch (r) {
        case kSwept: return "swept (gesture dirty)";
        case kUnion: return "union since last bake";
        case kAnchor: return "anchor footprint";
        default: return "?";
    }
}

void patch_expand(Patch* acc, const Patch& p, bool* seeded) {
    if (!*seeded) { *acc = p; *seeded = true; return; }
    for (int a = 0; a < 3; ++a) {
        acc->min[a] = std::min(acc->min[a], p.min[a]);
        acc->max[a] = std::max(acc->max[a], p.max[a]);
    }
}

struct Rays {
    double ms = 0.0;
    int hits = 0;
    int cast = 0;
};

Rays raycast(const clay_document* doc) {
    {   // warm: the first trace pays tape compile and pool spin-up
        int32_t h = 0; float t = 0, p[3], n[3];
        const float o[3] = {0, 0, 4}, dd[3] = {0, 0, -1};
        for (int w = 0; w < 8; ++w) clay_raycast(doc, o, dd, &h, &t, p, n);
    }
    Rays r;
    const Clock::time_point t0 = Clock::now();
    for (int i = 0; i < kRaySide; ++i)
        for (int j = 0; j < kRaySide; ++j) {
            const float u = -1.3f + 2.6f * static_cast<float>(i) / (kRaySide - 1);
            const float v = -1.3f + 2.6f * static_cast<float>(j) / (kRaySide - 1);
            const float o[3] = {u, v, 4.0f}, dd[3] = {0, 0, -1};
            int32_t hit = 0; float t = 0, p[3], n[3];
            if (clay_raycast(doc, o, dd, &hit, &t, p, n) == CLAY_OK) r.hits += hit ? 1 : 0;
            ++r.cast;
        }
    r.ms = ms_since(t0);
    return r;
}

float step_scale(const clay_document* doc, clay_layer_id layer, int* out_chain, int* out_items) {
    clay_field_report fr;
    std::memset(&fr, 0, sizeof fr);
    fr.struct_size = sizeof fr;
    if (clay_layer_field_report(doc, layer, 0.5f, &fr) != CLAY_OK) return -1.0f;
    if (out_chain) *out_chain = fr.longest_deformer_chain;
    if (out_items) *out_items = fr.item_count;
    return fr.safe_step_scale;
}

void params_at(clay_consolidation_params* cp, float cell) {
    std::memset(cp, 0, sizeof *cp);
    cp->struct_size = sizeof *cp;
    cp->cell_size = cell;
    cp->skip_redistance = 0;  // redistancing is what bounds the Lipschitz
}

struct RegionResult {
    bool ok = false;
    double plan_ms = 0.0;
    double bake_ms = 0.0;
    double closure_ratio = 0.0;
    unsigned long long absorbed = 0;
    bool whole_layer = false;
    unsigned long long bytes = 0;
};

RegionResult merge_patch(Doc* d, const Patch& p, float cell) {
    RegionResult r;
    clay_region_merge plan;
    std::memset(&plan, 0, sizeof plan);
    plan.struct_size = sizeof plan;
    Clock::time_point t0 = Clock::now();
    const clay_result pr = clay_layer_plan_region_merge(d->doc, d->layer, p.min, p.max, &plan);
    r.plan_ms = ms_since(t0);
    if (pr == CLAY_OK) {
        const double requested = box_volume(p.min, p.max);
        const double closure = box_volume(plan.box_min, plan.box_max);
        r.closure_ratio = requested > 0.0 ? closure / requested : 0.0;
        r.absorbed = static_cast<unsigned long long>(plan.absorbed);
        r.whole_layer = plan.whole_layer != 0;
    }
    clay_consolidation_params cp;
    params_at(&cp, cell);
    clay_consolidation_cost cost;
    std::memset(&cost, 0, sizeof cost);
    cost.struct_size = sizeof cost;
    t0 = Clock::now();
    const clay_result br =
        clay_layer_consolidate_region(d->doc, d->layer, p.min, p.max, &cp, &cost, nullptr);
    r.bake_ms = ms_since(t0);
    r.ok = (br == CLAY_OK);
    r.bytes = static_cast<unsigned long long>(cost.bytes);
    return r;
}

}  // namespace

int main() {
    std::printf("move_regional_probe: is clay_layer_consolidate_region the Move cure?\n");
    std::printf("  move radius %.2f, %d rays, cell %.3f\n\n", static_cast<double>(kMoveRadius),
                kRaySide * kRaySide, static_cast<double>(kCell));

    int failures = 0;

    // ---------------------------------------------------------------- ARMS A/B/C/D
    for (int topo = 0; topo < kTopologyCount; ++topo) {
        std::printf("=== %s ===\n", topology_name(topo));
        std::printf("  N | A raw: step  ray ms  hits | closure/req per region choice: swept  union  anchor (W=whole) | C bake ms  ray ms  hits | D +1 move ray\n");
        for (const int n : {1, 2, 4, 8, 16, 32}) {
            // ARM A -- raw chain
            Doc a;
            if (!build(&a, topo)) { std::printf("  FAIL build\n"); return 1; }
            Patch last{}, uni{}; bool seeded = false;
            for (int i = 0; i < n; ++i) { if (!dab(&a, i, &last)) { std::printf("  FAIL dab\n"); return 1; } patch_expand(&uni, last, &seeded); }
            int chain = 0, items = 0;
            const float a_step = step_scale(a.doc, a.layer, &chain, &items);
            const Rays a_rays = raycast(a.doc);

            // ARM C -- the same chain, then ONE regional merge on the patch
            Doc c;
            if (!build(&c, topo)) { std::printf("  FAIL build\n"); return 1; }
            Patch clast{}, cuni{}; bool cseeded = false;
            for (int i = 0; i < n; ++i) { dab(&c, i, &clast); patch_expand(&cuni, clast, &cseeded); }
            // The three region choices, PLANNED on the same chain before any
            // of them bakes — plan_region_merge changes nothing, so all three
            // ratios are measured against an identical document.
            Patch anchor_box = clast;
            for (int ax = 0; ax < 3; ++ax) {   // the footprint without the travel
                const float mid = 0.5f * (clast.min[ax] + clast.max[ax]);
                anchor_box.min[ax] = mid - kMoveRadius;
                anchor_box.max[ax] = mid + kMoveRadius;
            }
            const Patch choices[kRegionPolicyCount] = {clast, cuni, anchor_box};
            double ratio[kRegionPolicyCount] = {0, 0, 0};
            int whole[kRegionPolicyCount] = {0, 0, 0};
            for (int rp = 0; rp < kRegionPolicyCount; ++rp) {
                clay_region_merge pl;
                std::memset(&pl, 0, sizeof pl);
                pl.struct_size = sizeof pl;
                if (clay_layer_plan_region_merge(c.doc, c.layer, choices[rp].min,
                                                 choices[rp].max, &pl) == CLAY_OK) {
                    const double req = box_volume(choices[rp].min, choices[rp].max);
                    ratio[rp] = req > 0.0 ? box_volume(pl.box_min, pl.box_max) / req : 0.0;
                    whole[rp] = pl.whole_layer;
                }
            }

            // Bake with the SWEPT box — the tightest honest one, and what a
            // host would pass unless the ratios argue otherwise.
            const RegionResult cm = merge_patch(&c, clast, kCell);
            const Rays c_rays = cm.ok ? raycast(c.doc) : Rays{};

            // ARM D -- and then another Move on top of the baked patch
            double d_ray = -1.0;
            if (cm.ok) {
                Patch tmp{};
                if (dab(&c, n + 1, &tmp)) d_ray = raycast(c.doc).ms;
            }

            std::printf("  %2d | %11.6f %7.2f %5d | %8.2fx%s %7.2fx%s %7.2fx%s | %9.1f %7.2f %5d | %12.2f\n",
                        n, static_cast<double>(a_step), a_rays.ms, a_rays.hits,
                        ratio[kSwept], whole[kSwept] ? "W" : " ",
                        ratio[kUnion], whole[kUnion] ? "W" : " ",
                        ratio[kAnchor], whole[kAnchor] ? "W" : " ",
                        cm.bake_ms, c_rays.ms, c_rays.hits, d_ray);

            if (n == 1 && a_rays.hits == 0) {
                std::printf("  FAIL: the reference arm found no surface at N=1, so this fixture\n"
                            "        measures nothing.\n");
                ++failures;
            }
            if (cm.ok && c_rays.hits == 0) {
                std::printf("  NOTE: the BAKED arm found no surface -- the bake is wrong here,\n"
                            "        not merely slow. Do not read its timing as a result.\n");
                ++failures;
            }
            clay_document_destroy(a.doc);
            clay_document_destroy(c.doc);
        }
        std::printf("\n");
    }

    // ---------------------------------------------------------------- ARM E
    //
    // The arm that answers the real question: does interactive cost stay
    // BOUNDED while the artist keeps working the same patch? Three policies
    // over the same 48 gestures.
    std::printf("=== ARM E: repeated maintenance, 48 gestures on one patch (%s) ===\n",
                topology_name(kSphere));
    std::printf("  region passed to each bake: %s -- the union accumulated since the\n"
                "  last maintenance, reset after it, which is what makes 'bake rarely over a\n"
                "  loose box' a different proposition from 'bake often over a tight one'.\n",
                region_name(kUnion));
    struct Policy { const char* name; int every; float below; };
    const Policy policies[] = {
        {"no maintenance", 0, 0.0f},
        {"regional every 4", 4, 0.0f},
        {"regional when step < 0.30", 0, 0.30f},
    };
    for (const Policy& pol : policies) {
        Doc e;
        if (!build(&e, kSphere)) { std::printf("  FAIL build\n"); return 1; }
        std::printf("  %-28s | gesture  step_scale  items  ray ms  hits  merges  closure/req\n",
                    pol.name);
        int merges = 0;
        Patch eacc{};
        bool eseeded = false;
        double last_ratio = 0.0;
        for (int g = 1; g <= 48; ++g) {
            Patch gd{};
            if (!dab(&e, g, &gd)) break;
            patch_expand(&eacc, gd, &eseeded);
            bool merge = false;
            if (pol.every > 0 && (g % pol.every) == 0) merge = true;
            if (pol.below > 0.0f) {
                int ch = 0, it = 0;
                if (step_scale(e.doc, e.layer, &ch, &it) < pol.below) merge = true;
            }
            if (merge && eseeded) {
                // Policy (2): the union of every dirty region since the last
                // maintenance. Reset after, so the next bake sees only new work
                // -- which is what makes "bake rarely over a loose box" a
                // different proposition from "bake often over a tight one".
                const RegionResult rr = merge_patch(&e, eacc, kCell);
                if (rr.ok) {
                    ++merges;
                    last_ratio = rr.closure_ratio;
                    eseeded = false;
                }
            }
            if (g == 1 || g == 4 || g == 8 || g == 16 || g == 32 || g == 48) {
                int ch = 0, it = 0;
                const float s = step_scale(e.doc, e.layer, &ch, &it);
                const Rays r = raycast(e.doc);
                std::printf("  %-28s | %7d  %10.6f %6d %7.2f %5d %7d %12.2fx\n", "", g,
                            static_cast<double>(s), it, r.ms, r.hits, merges, last_ratio);
            }
        }
        clay_document_destroy(e.doc);
    }

    std::printf("\n  READ closure/req FIRST. Near 1-2x with `whole` = no means the closure stays\n"
                "  local and this mechanism is the cure. Routine `whole` = YES, or ratios in the\n"
                "  tens, means it exists and is not the practical answer for that form.\n");
    if (failures) {
        std::printf("\n%d invariant(s) failed; do not quote the tables above.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
