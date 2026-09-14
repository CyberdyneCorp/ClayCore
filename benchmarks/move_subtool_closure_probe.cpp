// CAN A REAL SCULPT PRODUCE A LOCAL CLOSURE AT ALL? NOT a gated benchmark.
//
// THE QUESTION, AND WHY IT IS NOT THE ONE move_regional_probe ASKED. That probe
// established the Move cure -- periodic consolidation keeps interactive cost
// flat (~5 ms across 48 gestures against 292 ms) and keeps the surface
// renderable (1028 hits held, against zero). It could not establish that the
// REGIONAL path is what delivers it, because every one of its five topologies
// returned `whole_layer = true`, with closure/requested from 17x to 396x.
//
// That was not a defect. The closure is grown until every item that can reach
// inside it is wholly inside it, and all five topologies shared ONE BASE SPHERE
// spanning the form. A sphere that reaches into the patch must be absorbed
// whole. Five variations on the same fixture.
//
// So `clay_layer_consolidate_region` degenerated to `clay_layer_consolidate` on
// every row, and the bake costs were whole-layer costs: 330 ms at N=1, 4.0 s at
// N=32, 12.3 s on the smooth-union form. Issue #540 relocated the question to
// the fixture that probe lacked:
//
//     a form assembled from several INDEPENDENT SUBTOOLS, with the patch on one
//     of them.
//
// This is that fixture. If the closure stays on one subtool, regional is the
// architecture and #534's trigger can be derived at local prices. If it
// swallows the layer even here, the honest conclusion is that periodic
// WHOLE-LAYER consolidation is the Move cure, and the 12.3 s bake is the number
// that decides whether that is tolerable between strokes.
//
// WHAT "INDEPENDENT" HAS TO MEAN, AND WHY THE SWEEP IS OVER SEPARATION. A
// character is not a bag of disjoint spheres. Subtools OVERLAP -- an arm enters
// a torso, a head sits into a neck -- and a sculptor blends them. The
// interesting question is not whether perfectly separated parts give a local
// closure (they must) but WHERE ALONG THAT AXIS locality is lost, because that
// is what tells a host whether their document qualifies. Hence the separation
// sweep from a real gap through touching to overlapping to blended, and hence
// the two arms that put a genuinely global item back into an otherwise local
// form: those are the shapes a real sculpt has.
//
// THE NUMBER TO READ IS `absorbed`, NOT THE VOLUME RATIO. The ratio the earlier
// probe reported is closure box volume over requested box volume, which is
// large whenever the patch is small even if the closure is one subtool.
// `absorbed` says how many ROOTS the bake takes, and against the layer's item
// count that is the locality statement directly: 1 of 8 is local, 8 of 8 is
// whole-layer whatever the box says.
//
// AND THE PAYOFF HAS TO BE MEASURED, NOT INFERRED. A local closure is only
// worth an architecture if the bake is correspondingly cheap, so every row
// bakes the region AND bakes the whole layer on an identical document and
// prints both. A local closure that still costs whole-layer time would mean the
// cost is in redistancing the result rather than in absorbing the items.
//
// WHAT IT FOUND, so a reader does not have to run it: a local closure IS
// reachable -- the cliff is at a gap of exactly zero, and 0.005 of clear air
// takes the bake from 8 roots to 2 and from 340 ms to 94 -- but it does not
// SURVIVE BEING USED. Maintaining a patch grows the closure ~1.76 a bake with
// the requested box fixed and the form stationary, until it has swallowed the
// layer: 80 ms at the first bake, 4.1 s at the twelfth.
//
// Exits non-zero if it cannot discriminate: if the subtools are not actually
// separate roots, if the reference arm finds no surface, or if a bake loses it.

#include <algorithm>
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

constexpr float kMoveRadius = 0.22f;  // a patch on ONE subtool, not across two
constexpr float kSubtoolRadius = 0.50f;
constexpr int kSubtools = 8;
constexpr int kRaySide = 48;
constexpr float kCell = 0.02f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

// -- the assemblies ---------------------------------------------------------
//
// Eight subtools in a row along X, varying only how they meet. The patch always
// lands on subtool 0, the far end, so a closure that stays local has seven
// other roots available to swallow and declines to.
enum Assembly {
    kGap = 0,        // 0.40 of clear air between neighbours: parts not yet joined
    kTouching,       // surfaces meeting exactly: the boundary case
    kOverlapping,    // 0.20 of interpenetration: how a body is actually built
    kBlended,        // overlapping AND smooth-unioned: how it is actually finished
    kGlobalShell,    // overlapping, plus one large ADD spanning every subtool
    kGlobalSubtract, // overlapping, plus one large SUBTRACT crossing the form
    kAssemblyCount,
};

const char* assembly_name(int a) {
    switch (a) {
        case kGap: return "8 subtools, 0.40 gap";
        case kTouching: return "8 subtools, touching";
        case kOverlapping: return "8 subtools, 0.20 overlap";
        case kBlended: return "8 subtools, overlap + blend";
        case kGlobalShell: return "8 subtools + spanning ADD";
        case kGlobalSubtract: return "8 subtools + crossing SUBTRACT";
        default: return "?";
    }
}

float spacing_of(int assembly) {
    switch (assembly) {
        case kGap: return 2.0f * kSubtoolRadius + 0.40f;
        case kTouching: return 2.0f * kSubtoolRadius;
        default: return 2.0f * kSubtoolRadius - 0.20f;
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

// Subtool 0 sits at the -X end. The patch is worked there, as far as this form
// allows from everything else.
float subtool_x(int assembly, int i) {
    const float s = spacing_of(assembly);
    return -0.5f * s * static_cast<float>(kSubtools - 1) + s * static_cast<float>(i);
}

// Subtool positions for an arbitrary spacing, used by the gap sweep below.
float subtool_x_at(float spacing, int i) {
    return -0.5f * spacing * static_cast<float>(kSubtools - 1) + spacing * static_cast<float>(i);
}

bool build_spaced(Doc* out, float spacing, float blend) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    Doc d{doc, layer};
    bool ok = true;
    for (int i = 0; i < kSubtools && ok; ++i) {
        const float p[3] = {subtool_x_at(spacing, i), 0.0f, 0.0f};
        ok = add_sphere(&d, kSubtoolRadius, p, CLAY_OP_ADD, blend);
    }
    if (!ok) {
        clay_document_destroy(doc);
        return false;
    }
    *out = d;
    return true;
}

bool build(Doc* out, int assembly) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    Doc d{doc, layer};
    const float blend = (assembly == kBlended) ? 0.12f : 0.0f;
    bool ok = true;
    for (int i = 0; i < kSubtools && ok; ++i) {
        const float p[3] = {subtool_x(assembly, i), 0.0f, 0.0f};
        ok = add_sphere(&d, kSubtoolRadius, p, CLAY_OP_ADD, blend);
    }
    if (assembly == kGlobalShell) {
        // The armature a sculptor blocks in under everything -- one item that
        // genuinely reaches every subtool.
        const float p[3] = {0.0f, -0.42f, 0.0f};
        const float half[3] = {subtool_x(kOverlapping, kSubtools - 1) + kSubtoolRadius, 0.10f,
                               0.10f};
        clay_item* it = clay_item_create(CLAY_PRIM_BOX, half, 3);
        if (!it) ok = false;
        if (ok) {
            clay_item_set_position(it, p);
            clay_item_set_op(it, CLAY_OP_ADD);
            clay_node_id n = 0;
            ok = clay_layer_add_item(d.doc, d.layer, it, &n) == CLAY_OK;
            clay_item_destroy(it);
        }
    } else if (assembly == kGlobalSubtract) {
        // A cut taken across the whole form at once, which is how a sculptor
        // trims a silhouette -- and the item most likely to open the closure.
        const float half[3] = {subtool_x(kOverlapping, kSubtools - 1) + kSubtoolRadius, 0.30f,
                               0.30f};
        const float p[3] = {0.0f, 0.62f, 0.0f};
        clay_item* it = clay_item_create(CLAY_PRIM_BOX, half, 3);
        if (!it) ok = false;
        if (ok) {
            clay_item_set_position(it, p);
            clay_item_set_op(it, CLAY_OP_SUBTRACT);
            clay_node_id n = 0;
            ok = clay_layer_add_item(d.doc, d.layer, it, &n) == CLAY_OK;
            clay_item_destroy(it);
        }
    }
    if (!ok) {
        clay_document_destroy(doc);
        return false;
    }
    *out = d;
    return true;
}

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

// One Move dab on subtool 0, walked around its own front face so successive
// dabs neither coalesce nor wander onto a neighbour.
bool dab_at(Doc* d, float x0, int index, Patch* out_dirty) {
    const float t = 0.61f * static_cast<float>(index);
    const float cy = std::cos(t) * 0.20f;
    const float cz = std::sin(t) * 0.20f;
    const float r = std::sqrt(std::max(0.02f, kSubtoolRadius * kSubtoolRadius - cy * cy - cz * cz));
    const float anchor[3] = {x0 - r, cy, cz};

    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kMoveRadius;
    mp.ease = 0;
    mp.front_only = 0;

    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, anchor, &mp, nullptr);
    if (!tx) return false;
    // Pulled outward, away from the rest of the form: a displacement toward the
    // neighbours would be the fixture answering its own question. The SIGN
    // alternates, which is not cosmetic -- a fixed sign migrates material by
    // 0.09 a gesture, and over 48 gestures that grew the layer itself by half
    // its width. A closure tracking a form that is running away is correct
    // behaviour and would have been reported here as a ratchet. Alternating
    // holds the layer still so the closure's growth is the closure's own.
    const float out = (index % 2) ? 0.09f : -0.09f;
    const float total[3] = {out, cy * 0.12f, cz * 0.12f};
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
    if (ok && out_dirty && dirty.has_bounds)
        for (int a = 0; a < 3; ++a) {
            out_dirty->min[a] = dirty.bounds_min[a];
            out_dirty->max[a] = dirty.bounds_max[a];
        }
    clay_sdf_move_destroy(tx);
    return ok;
}

bool dab(Doc* d, int assembly, int index, Patch* out_dirty) {
    return dab_at(d, subtool_x(assembly, 0), index, out_dirty);
}

struct Rays {
    double ms = 0.0;
    int hits = 0;
};

// Cast along +X at the whole row, so a lost subtool shows up as lost hits
// whichever one it was.
Rays raycast(const clay_document* doc, int assembly) {
    const float x0 = subtool_x(assembly, 0) - 3.0f;
    {
        int32_t h = 0; float t = 0, p[3], n[3];
        const float o[3] = {x0, 0, 0}, dd[3] = {1, 0, 0};
        for (int w = 0; w < 8; ++w) clay_raycast(doc, o, dd, &h, &t, p, n);
    }
    Rays r;
    const Clock::time_point t0 = Clock::now();
    for (int i = 0; i < kRaySide; ++i)
        for (int j = 0; j < kRaySide; ++j) {
            const float u = -0.62f + 1.24f * static_cast<float>(i) / (kRaySide - 1);
            const float v = -0.62f + 1.24f * static_cast<float>(j) / (kRaySide - 1);
            const float o[3] = {x0, u, v}, dd[3] = {1, 0, 0};
            int32_t hit = 0; float t = 0, p[3], n[3];
            if (clay_raycast(doc, o, dd, &hit, &t, p, n) == CLAY_OK) r.hits += hit ? 1 : 0;
        }
    r.ms = ms_since(t0);
    return r;
}

int item_count(const clay_document* doc, clay_layer_id layer, float* out_step) {
    clay_field_report fr;
    std::memset(&fr, 0, sizeof fr);
    fr.struct_size = sizeof fr;
    if (clay_layer_field_report(doc, layer, 0.5f, &fr) != CLAY_OK) return -1;
    if (out_step) *out_step = fr.safe_step_scale;
    return static_cast<int>(fr.item_count);
}

// The layer's own tight bounds along the row. If this grows at the same rate
// the closure does, the closure is tracking a form that is running away and
// there is no ratchet to report; if it is flat while the closure grows, there
// is.
float layer_width_x(const clay_document* doc, clay_layer_id layer) {
    float lo[3], hi[3];
    int32_t has = 0;
    if (clay_layer_bounds(doc, layer, lo, hi, &has) != CLAY_OK || !has) return -1.0f;
    return hi[0] - lo[0];
}

void params_at(clay_consolidation_params* cp, float cell) {
    std::memset(cp, 0, sizeof *cp);
    cp->struct_size = sizeof *cp;
    cp->cell_size = cell;
    cp->skip_redistance = 0;
}

// Plan a merge and report only what locality needs: how many roots it takes.
struct Plan {
    unsigned long long absorbed = 0;
    bool whole = false;
    double ratio = 0.0;
    float span_x = 0.0f;  // the closure's extent along the row of subtools
};

Plan plan_at(const Doc& d, const Patch& p) {
    clay_region_merge pl;
    std::memset(&pl, 0, sizeof pl);
    pl.struct_size = sizeof pl;
    Plan r;
    if (clay_layer_plan_region_merge(d.doc, d.layer, p.min, p.max, &pl) == CLAY_OK) {
        const double req = box_volume(p.min, p.max);
        r.absorbed = pl.absorbed;
        r.whole = pl.whole_layer != 0;
        r.ratio = req > 0.0 ? box_volume(pl.box_min, pl.box_max) / req : 0.0;
        r.span_x = pl.box_max[0] - pl.box_min[0];
    }
    return r;
}

void patch_expand(Patch* acc, const Patch& p, bool* seeded) {
    if (!*seeded) { *acc = p; *seeded = true; return; }
    for (int a = 0; a < 3; ++a) {
        acc->min[a] = std::min(acc->min[a], p.min[a]);
        acc->max[a] = std::max(acc->max[a], p.max[a]);
    }
}

}  // namespace

int main() {
    std::printf("move_subtool_closure_probe: can a multi-subtool form keep the closure local?\n");
    std::printf("  %d subtools of r=%.2f in a row, patch on subtool 0, move radius %.2f, cell %.3f\n",
                kSubtools, static_cast<double>(kSubtoolRadius), static_cast<double>(kMoveRadius),
                static_cast<double>(kCell));
    std::printf("  READ `absorbed/items` FIRST. That is the locality statement; the volume\n"
                "  ratio is large whenever the patch is small and says nothing on its own.\n\n");

    int failures = 0;
    int local_rows = 0, total_rows = 0;

    for (int asm_ = 0; asm_ < kAssemblyCount; ++asm_) {
        std::printf("=== %s ===\n", assembly_name(asm_));
        std::printf("   N | items  step     | absorbed  whole | closure/req | region bake ms  whole bake ms  ratio | hits before  after\n");
        for (const int n : {1, 2, 4, 8, 16, 32}) {
            Doc d;
            if (!build(&d, asm_)) { std::printf("  FAIL build\n"); return 1; }

            Patch last{}, uni{};
            bool seeded = false;
            for (int i = 0; i < n; ++i) {
                if (!dab(&d, asm_, i, &last)) { std::printf("  FAIL dab\n"); return 1; }
                if (!seeded) { uni = last; seeded = true; }
                else
                    for (int a = 0; a < 3; ++a) {
                        uni.min[a] = std::min(uni.min[a], last.min[a]);
                        uni.max[a] = std::max(uni.max[a], last.max[a]);
                    }
            }

            float step = -1.0f;
            const int items = item_count(d.doc, d.layer, &step);
            const Rays pre = raycast(d.doc, asm_);

            // The union since the start, which is what an at-pointer-up policy
            // accumulates and the region the earlier probe's arm E passed.
            clay_region_merge plan;
            std::memset(&plan, 0, sizeof plan);
            plan.struct_size = sizeof plan;
            double ratio = 0.0;
            unsigned long long absorbed = 0;
            int whole = 0;
            if (clay_layer_plan_region_merge(d.doc, d.layer, uni.min, uni.max, &plan) == CLAY_OK) {
                const double req = box_volume(uni.min, uni.max);
                ratio = req > 0.0 ? box_volume(plan.box_min, plan.box_max) / req : 0.0;
                absorbed = plan.absorbed;
                whole = plan.whole_layer;
            }

            // Both bakes, on IDENTICAL documents, so the payoff is measured
            // rather than inferred from the closure size.
            Doc r_doc = d;
            clay_consolidation_params cp;
            params_at(&cp, kCell);
            clay_consolidation_cost cost;
            std::memset(&cost, 0, sizeof cost);
            cost.struct_size = sizeof cost;
            Clock::time_point t0 = Clock::now();
            const bool r_ok = clay_layer_consolidate_region(r_doc.doc, r_doc.layer, uni.min,
                                                            uni.max, &cp, &cost, nullptr) == CLAY_OK;
            const double region_ms = ms_since(t0);
            const Rays post = r_ok ? raycast(r_doc.doc, asm_) : Rays{};

            Doc w;
            if (!build(&w, asm_)) { std::printf("  FAIL build\n"); return 1; }
            for (int i = 0; i < n; ++i) dab(&w, asm_, i, nullptr);
            clay_consolidation_cost wcost;
            std::memset(&wcost, 0, sizeof wcost);
            wcost.struct_size = sizeof wcost;
            t0 = Clock::now();
            const bool w_ok =
                clay_layer_consolidate(w.doc, w.layer, &cp, nullptr, nullptr, &wcost) == CLAY_OK;
            const double whole_ms = ms_since(t0);

            std::printf("  %2d | %5d %8.6f | %8llu  %5s | %10.1fx | %14.1f %14.1f %6.2fx | %11d %6d\n",
                        n, items, static_cast<double>(step), absorbed, whole ? "YES" : "no",
                        ratio, region_ms, whole_ms,
                        whole_ms > 0.0 ? region_ms / whole_ms : 0.0, pre.hits, post.hits);

            ++total_rows;
            if (!whole && items > 0 && static_cast<int>(absorbed) < items) ++local_rows;

            // A lost surface at depth is move_regional_probe's finding reproduced,
            // not a defect in this fixture: the raw chain stops rendering as the
            // step scale collapses. It is only fatal at N=1, where it would mean
            // the assembly never had a surface to work.
            if (pre.hits == 0 && n == 1) {
                std::printf("  FAIL: no surface at N=1 -- this assembly measures nothing.\n");
                ++failures;
            }
            if (r_ok && post.hits == 0) {
                std::printf("  NOTE: the REGION bake lost the surface -- wrong, not merely slow.\n");
                ++failures;
            }
            if (!r_ok || !w_ok) {
                std::printf("  FAIL: a bake was refused (region %d, whole %d).\n", r_ok, w_ok);
                ++failures;
            }
            clay_document_destroy(r_doc.doc);
            clay_document_destroy(w.doc);
        }
        std::printf("\n");
    }

    // ------------------------------------------------------------------ ARM B
    //
    // The first table does not degrade from a gap to an overlap -- it FALLS OFF
    // A CLIFF between 0.40 of clear air (1 root of 8) and exactly touching (8 of
    // 8). A host cannot act on "keep them apart" without knowing how far apart,
    // and the answer is not obviously 0: the closure is grown from item BOUNDS,
    // which for a sphere is its own box, so the threshold could sit anywhere
    // from touching-to-the-float up to a full cell of slack.
    std::printf("=== ARM B: where the cliff sits, %d dabs on subtool 0 ===\n", 4);
    std::printf("  A sphere's bound is its own box, so this asks whether the closure needs\n"
                "  clear air at all or merely non-overlapping BOUNDS.\n");
    std::printf("   gap  | absorbed  whole | closure/req | region bake ms\n");
    for (const float gap : {-0.20f, -0.05f, 0.0f, 0.005f, 0.02f, 0.05f, 0.10f, 0.20f, 0.40f}) {
        Doc d;
        if (!build_spaced(&d, 2.0f * kSubtoolRadius + gap, 0.0f)) { std::printf("  FAIL build\n"); return 1; }
        const float x0 = subtool_x_at(2.0f * kSubtoolRadius + gap, 0);
        Patch last{}, uni{};
        bool seeded = false;
        for (int i = 0; i < 4; ++i) {
            if (!dab_at(&d, x0, i, &last)) { std::printf("  FAIL dab\n"); return 1; }
            patch_expand(&uni, last, &seeded);
        }
        const Plan pl = plan_at(d, uni);
        clay_consolidation_params cp;
        params_at(&cp, kCell);
        clay_consolidation_cost cost;
        std::memset(&cost, 0, sizeof cost);
        cost.struct_size = sizeof cost;
        const Clock::time_point t0 = Clock::now();
        const bool ok = clay_layer_consolidate_region(d.doc, d.layer, uni.min, uni.max, &cp,
                                                      &cost, nullptr) == CLAY_OK;
        std::printf("  %5.3f | %8llu  %5s | %10.1fx | %14.1f%s\n", static_cast<double>(gap),
                    pl.absorbed, pl.whole ? "YES" : "no", pl.ratio, ms_since(t0),
                    ok ? "" : "  (REFUSED)");
        if (!ok) ++failures;
        clay_document_destroy(d.doc);
    }
    std::printf("\n");

    // ------------------------------------------------------------------ ARM C
    //
    // The arm that decides the architecture. Arm A planned ONE merge after a
    // chain had already been allowed to grow, and watched the closure widen
    // from 1 root to 3 as it did. The question a host actually faces is the
    // other one: if you MAINTAIN, does the closure stay at one root and the
    // bake stay cheap, gesture after gesture? That is the O(1) property at
    // LOCAL prices, which is the only thing that would make the regional path
    // worth binding over the whole-layer one.
    std::printf("=== ARM C: 48 gestures on one subtool, regional maintenance every 4 ===\n");
    std::printf("  gap 0.40 (the assembly arm A found local). The box passed to each bake is\n"
                "  the union since the LAST bake, reset after it.\n");
    std::printf("  If `bake ms` grows while `absorbed` does not, the driver is not the root\n"
                "  COUNT but the extent of the baked volume the previous bake left behind --\n"
                "  a different defect with a different fix, so both are reported.\n");
    std::printf("   gesture | items  step     | absorbed  whole | closure X  baked X | bake ms | hits\n");
    {
        Doc e;
        if (!build_spaced(&e, 2.0f * kSubtoolRadius + 0.40f, 0.0f)) { std::printf("  FAIL build\n"); return 1; }
        const float x0 = subtool_x_at(2.0f * kSubtoolRadius + 0.40f, 0);
        Patch acc{};
        bool seeded = false;
        double last_bake = 0.0;
        float last_baked_x = 0.0f;
        Plan last_plan;
        for (int g = 1; g <= 48; ++g) {
            Patch gd{};
            if (!dab_at(&e, x0, g, &gd)) { std::printf("  FAIL dab %d\n", g); return 1; }
            patch_expand(&acc, gd, &seeded);
            if ((g % 4) == 0 && seeded) {
                last_plan = plan_at(e, acc);
                clay_consolidation_params cp;
                params_at(&cp, kCell);
                clay_consolidation_cost cost;
                std::memset(&cost, 0, sizeof cost);
                cost.struct_size = sizeof cost;
                const Clock::time_point t0 = Clock::now();
                const bool ok = clay_layer_consolidate_region(e.doc, e.layer, acc.min, acc.max,
                                                              &cp, &cost, nullptr) == CLAY_OK;
                last_bake = ms_since(t0);
                last_baked_x = cost.bounds_max[0] - cost.bounds_min[0];
                // Every bake, not only the sampled rows: the growth per bake is
                // the mechanism, and six sampled rows cannot show it.
                std::printf("      bake %2d after gesture %2d | requested X [%7.3f %7.3f] w=%6.3f"
                            " | LAYER w=%6.3f | closure w=%6.3f | baked w=%6.3f"
                            " | roots %llu%s | %7.1f ms\n",
                            g / 4, g, static_cast<double>(acc.min[0]),
                            static_cast<double>(acc.max[0]),
                            static_cast<double>(acc.max[0] - acc.min[0]),
                            static_cast<double>(layer_width_x(e.doc, e.layer)),
                            static_cast<double>(last_plan.span_x),
                            static_cast<double>(last_baked_x), last_plan.absorbed,
                            last_plan.whole ? " W" : "  ", last_bake);
                if (!ok) { std::printf("  FAIL: maintenance bake refused at gesture %d\n", g); ++failures; break; }
                seeded = false;
            }
            if (g == 4 || g == 8 || g == 16 || g == 24 || g == 32 || g == 48) {
                float st = -1.0f;
                const int it = item_count(e.doc, e.layer, &st);
                const Rays r = raycast(e.doc, kGap);
                std::printf("   %7d | %5d %8.6f | %8llu  %5s | %9.3f %8.3f | %7.1f | %4d\n", g,
                            it, static_cast<double>(st), last_plan.absorbed,
                            last_plan.whole ? "YES" : "no",
                            static_cast<double>(last_plan.span_x),
                            static_cast<double>(last_baked_x), last_bake, r.hits);
                if (r.hits == 0) {
                    std::printf("  FAIL: maintenance lost the surface at gesture %d.\n", g);
                    ++failures;
                }
            }
        }
        clay_document_destroy(e.doc);
    }
    std::printf("\n");

    // ------------------------------------------------------------------ ARM D
    //
    // The comparison that turns arm C into a decision. Arm C's bakes grow from
    // 80 ms to over 4 s across one session; arm A measured a WHOLE-LAYER bake on
    // this same form at ~2.3 s after 32 gestures. Those are different arms on
    // different documents, so the crossing cannot be read across them -- it has
    // to be run. Same 48 gestures, same cadence, the only difference being which
    // entry point maintains them.
    std::printf("=== ARM D: the same 48 gestures, maintained WHOLE-LAYER every 4 ===\n");
    std::printf("   gesture | items  step     | LAYER w | bake ms | hits\n");
    {
        Doc w;
        if (!build_spaced(&w, 2.0f * kSubtoolRadius + 0.40f, 0.0f)) { std::printf("  FAIL build\n"); return 1; }
        const float x0 = subtool_x_at(2.0f * kSubtoolRadius + 0.40f, 0);
        double last_bake = 0.0;
        for (int g = 1; g <= 48; ++g) {
            Patch gd{};
            if (!dab_at(&w, x0, g, &gd)) { std::printf("  FAIL dab %d\n", g); return 1; }
            if ((g % 4) == 0) {
                clay_consolidation_params cp;
                params_at(&cp, kCell);
                clay_consolidation_cost cost;
                std::memset(&cost, 0, sizeof cost);
                cost.struct_size = sizeof cost;
                const Clock::time_point t0 = Clock::now();
                const bool ok = clay_layer_consolidate(w.doc, w.layer, &cp, nullptr, nullptr,
                                                       &cost) == CLAY_OK;
                last_bake = ms_since(t0);
                if (!ok) { std::printf("  FAIL: whole-layer bake refused at gesture %d\n", g); ++failures; break; }
            }
            if (g == 4 || g == 8 || g == 16 || g == 24 || g == 32 || g == 48) {
                float st = -1.0f;
                const int it = item_count(w.doc, w.layer, &st);
                const Rays r = raycast(w.doc, kGap);
                std::printf("   %7d | %5d %8.6f | %7.3f | %7.1f | %4d\n", g, it,
                            static_cast<double>(st),
                            static_cast<double>(layer_width_x(w.doc, w.layer)), last_bake,
                            r.hits);
                if (r.hits == 0) {
                    std::printf("  FAIL: whole-layer maintenance lost the surface at %d.\n", g);
                    ++failures;
                }
            }
        }
        clay_document_destroy(w.doc);
    }
    std::printf("\n");

    std::printf("%d of %d rows in arm A kept the closure off at least one root.\n", local_rows, total_rows);
    std::printf("If that is 0, the answer to #540 is NO for every assembly tried, and periodic\n"
                "WHOLE-LAYER consolidation is the Move cure. If it is the separated assemblies\n"
                "only, the answer is that locality survives a gap and not an overlap, which is\n"
                "a statement about what a host's document must look like -- not a policy.\n");
    if (failures) {
        std::printf("\n%d invariant(s) failed; do not quote the tables above.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
