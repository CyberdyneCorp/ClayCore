// The measurement behind issue #207 candidate 3, kept so its table can be
// re-derived rather than trusted. NOT a gated benchmark.
//
// The claim under test: `ctape_prim_dist` is "a ~35-way indirect branch per
// instruction, well predicted for a uniform tape, less so for a mixed one".
// On arm64 the first half is literally true -- AppleClang compiles the if-chain
// to a real jump table (`ldrh` from lJTI, `br`), out of line, behind a prologue
// that spills d8-d15 and x19-x28 on every prim instruction including a sphere's
// six flops. So chain POSITION costs nothing and only the branch is in question.
//
// THE DESIGN. Four tapes at the SAME instruction count, over the SAME points,
// with the SAME transforms, the SAME positions and the SAME hard-add combines.
// Only the opcode sequence differs:
//
//   U_k      every prim is opcode k         (K tapes, one per primitive)
//   CYCLIC   round-robin over the K         (same histogram, period K)
//   BLOCKED  all of k=0, then all of k=1    (same histogram, K-1 target changes)
//   RANDOM   a fixed shuffle                (same histogram, aperiodic)
//
// mean(U_k) is the ARITHMETIC-MATCHED prediction for the mixed tapes: it charges
// each primitive its own uniform cost in exactly the proportion the mixed tapes
// use it. So the ratios read:
//
//   CYCLIC  / mean(U)  the mixed FOOTPRINT, with the order learnable
//   RANDOM  / CYCLIC   the opcode ORDER alone -- identical code, identical work,
//                      identical histogram, identical param layout. The one
//                      clean separation of misprediction from footprint here.
//   BLOCKED / CYCLIC   THE CONTROL, and the reason RANDOM/CYCLIC is readable.
//                      `ctape_smin_m`'s hard arm is a data-dependent select, so
//                      a mixed tape changes the (a < b) outcome sequence as well
//                      as the opcode sequence. BLOCKED pairs shapes to positions
//                      as differently from CYCLIC as RANDOM does while keeping
//                      dispatch maximally predictable. It reads at or below
//                      CYCLIC at every size and every K -- so the data is not
//                      the mechanism and the RANDOM excess is the branch.
//
// Tape SIZE is swept because a tape is a fixed sequence replayed once per point:
// a short shuffle is still learnable, so misprediction appears only once the
// tape outruns the predictor. It does, between 511 and 2047 instructions.
// K is swept because the effect is nearly saturated at K=2, which is what rules
// out icache: at K=2 the footprint is two arms and CYCLIC/mean(U) is 1.001x,
// while RANDOM still costs 1.14x.
//
// Every arm is timed twice, in the same interleaved round: through the shipping
// `ctape_eval` (V0) and through a stripped distance-only loop (lean), because
// dispatch is a much larger fraction of the lean shape the blocked evaluator
// uses. Two further arms bound the total on the uniform sphere tape --
// DISPATCHED via `ctape_prim_dist` against DIRECT `sd_sphere` -- and their ratio
// is dispatch PLUS the out-of-line call PLUS its prologue, which this cannot
// split. Both are asserted bit-identical to `ctape_eval`; that guard is what
// makes the timings a comparison rather than two different computations.
//
// WHAT IT CANNOT SEPARATE. BTB/indirect-target capacity from predictor history
// length -- both are "misprediction". And the loads that precede dispatch are
// byte-identical across all four tapes, so they sit entirely inside mean(U) and
// are not sized here at all.
//
//   ./prim_dispatch_probe [items] [points] [rounds] [kinds] [shuffle-seed]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::kernel;
using kernel::cf3;

namespace {

std::uint64_t next(std::uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s;
}
float unit(std::uint64_t& s) {
    return static_cast<float>((next(s) >> 40) & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f;
}

std::vector<float> make_points(std::size_t count) {
    std::vector<float> p(count * 3);
    std::uint64_t s = 99991;
    for (float& v : p) v = unit(s);
    return p;
}

// Pure-analytic primitives only: no blob, no deformer, no repeat, so every tape
// below differs from every other ONLY in which switch arm each prim takes.
// Spread across the opcode range, and all about the same size so the distances
// entering the combines have the same distribution.
struct PrimKind {
    const char* name;
    int op;  // CTapeOp, for the record
    scene::Prim (*make)();
};

const PrimKind kKinds[] = {
    {"sphere", 0, [] { return scene::Prim::sphere(0.12f); }},
    {"box", 1, [] { return scene::Prim::box(cf3(0.10f, 0.11f, 0.12f)); }},
    {"round_box", 2, [] { return scene::Prim::round_box(cf3(0.09f, 0.10f, 0.11f), 0.02f); }},
    {"box_frame", 3, [] { return scene::Prim::box_frame(cf3(0.10f, 0.11f, 0.12f), 0.02f); }},
    {"torus", 4, [] { return scene::Prim::torus(0.09f, 0.03f); }},
    {"capped_cylinder", 6, [] { return scene::Prim::capped_cylinder(0.10f, 0.12f); }},
    {"rounded_cylinder", 7, [] { return scene::Prim::rounded_cylinder(0.08f, 0.02f, 0.10f); }},
    {"capped_cone", 8, [] { return scene::Prim::capped_cone(0.12f, 0.10f, 0.05f); }},
    {"round_cone", 9, [] { return scene::Prim::round_cone(0.09f, 0.04f, 0.10f); }},
    {"ellipsoid", 10, [] { return scene::Prim::ellipsoid(cf3(0.10f, 0.12f, 0.08f)); }},
    {"octahedron", 11, [] { return scene::Prim::octahedron(0.12f); }},
    {"hex_prism", 12, [] { return scene::Prim::hex_prism(0.10f, 0.11f); }},
    {"pyramid", 13, [] { return scene::Prim::pyramid(0.12f); }},
    {"capped_torus", 17, [] { return scene::Prim::capped_torus(1.0f, 0.09f, 0.03f); }},
    {"link", 18, [] { return scene::Prim::link(0.05f, 0.08f, 0.03f); }},
    {"cut_sphere", 22, [] { return scene::Prim::cut_sphere(0.12f, 0.03f); }},
    {"tetrahedron", 25, [] { return scene::Prim::tetrahedron(0.12f); }},
    {"dodecahedron", 26, [] { return scene::Prim::dodecahedron(0.12f); }},
    {"icosahedron", 27, [] { return scene::Prim::icosahedron(0.12f); }},
    {"tri_prism", 28, [] { return scene::Prim::tri_prism(0.10f, 0.11f); }},
    {"octahedron_cheap", 29, [] { return scene::Prim::octahedron_cheap(0.12f); }},
    {"lnorm_sphere", 30, [] { return scene::Prim::lnorm_sphere(0.12f, 4.0f); }},
};
constexpr std::size_t kNumKinds = sizeof(kKinds) / sizeof(kKinds[0]);

// `pick(i)` chooses the opcode for item i. Positions and combines are identical
// across every tape this builds, so instruction count matches exactly.
template <class Pick>
scene::Document make_document(std::size_t items, Pick pick) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("probe");
    std::uint64_t s = 4242;
    for (std::size_t i = 0; i < items; ++i) {
        scene::Node n;
        n.prim = kKinds[pick(i)].make();
        n.xform.position = cf3(unit(s) * 0.8f, unit(s) * 0.8f, unit(s) * 0.8f);
        n.op = scene::Op::Add;
        n.blend = scene::Blend{scene::BlendProfile::Hard, 0.0f};
        l.sdf->insert(n);
    }
    return doc;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
double cv_pct(const std::vector<double>& v) {
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    s = std::sqrt(s / static_cast<double>(v.size()));
    return 100.0 * s / m;
}

struct Arm {
    const char* name;
    const scene::Tape* tape;
    std::vector<double> ms;    // ctape_eval, the shipping interpreter
    std::vector<double> lean;  // stripped distance-only loop
};

double time_eval(const scene::Tape& t, const std::vector<float>& pts, std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i)
        out[i] = t.eval(cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2])).d;
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The two bounding arms. Same loop shape, one dispatches and one does not.
double time_dispatched(const scene::Tape& t, const std::vector<float>& pts,
                       std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const float* bl = t.blob.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float st[CLAY_TAPE_MAX_STACK];
        std::size_t top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                const float b = st[top - 1];
                const float a = st[top - 2];
                --top;
                st[top - 1] = ctape_smin_m(CLAY_INT(pr[1]), a, b, pr[2]).x;
            } else {
                cfloat4x4 inv;
                inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
                inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
                inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
                inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
                st[top++] = ctape_prim_dist(in[k].op, pr + CLAY_TAPE_PRIM_HEADER, bl,
                                            cmul_point(inv, p)) *
                                pr[12] - pr[13];
            }
        }
        out[i] = st[top - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double time_direct(const scene::Tape& t, const std::vector<float>& pts, std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float st[CLAY_TAPE_MAX_STACK];
        std::size_t top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                const float b = st[top - 1];
                const float a = st[top - 2];
                --top;
                st[top - 1] = ctape_smin_m(CLAY_INT(pr[1]), a, b, pr[2]).x;
            } else {
                cfloat4x4 inv;
                inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
                inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
                inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
                inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
                // No opcode read, no jump table, no call: the sphere directly.
                st[top++] = sd_sphere(cmul_point(inv, p), pr[CLAY_TAPE_PRIM_HEADER]) * pr[12] -
                            pr[13];
            }
        }
        out[i] = st[top - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t items = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 256;
    const std::size_t count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 20480;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 9;
    const std::size_t nkinds = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : kNumKinds;
    const std::uint64_t seed = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 777771ull;

    const std::vector<float> pts = make_points(count);

    // A fixed shuffle: the same multiset of opcodes as CYCLIC, in an order the
    // predictor cannot get from a short period.
    std::vector<int> perm(items);
    for (std::size_t i = 0; i < items; ++i) perm[i] = static_cast<int>(i % nkinds);
    {
        std::uint64_t s = seed;
        for (std::size_t i = items; i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(next(s) % i);
            std::swap(perm[i - 1], perm[j]);
        }
    }

    std::vector<scene::Tape> uniform;
    uniform.reserve(nkinds);
    for (std::size_t k = 0; k < nkinds; ++k)
        uniform.push_back(compile_document(make_document(items, [k](std::size_t) { return k; })));
    const scene::Tape cyclic =
        compile_document(make_document(items, [&](std::size_t i) { return i % nkinds; }));
    // BLOCKED: all of kind 0, then all of kind 1, ... Same histogram as CYCLIC and
    // RANDOM, and the dispatch target changes only nkinds-1 times in the whole
    // tape -- so it is the MOST predictable order there is, while pairing shapes
    // to positions as differently from CYCLIC as RANDOM does. It is the control
    // that separates "the opcode order" from "which shape sits at which point",
    // because the hard-blend combine's (a < b) select is data-dependent.
    const std::size_t per = (items + nkinds - 1) / nkinds;
    const scene::Tape blocked = compile_document(
        make_document(items, [&](std::size_t i) { return std::min(i / per, nkinds - 1); }));
    const scene::Tape random_t = compile_document(
        make_document(items, [&](std::size_t i) { return static_cast<std::size_t>(perm[i]); }));

    // Instruction counts must match or none of the ns/instruction figures are
    // comparable. Refuse to publish if they do not.
    const std::size_t ni = cyclic.instrs.size();
    bool matched = random_t.instrs.size() == ni && blocked.instrs.size() == ni;
    for (const scene::Tape& t : uniform) matched = matched && t.instrs.size() == ni;
    if (!matched) {
        std::printf("instruction counts do NOT match; comparison is invalid\n");
        for (std::size_t k = 0; k < nkinds; ++k)
            std::printf("  %-18s %zu\n", kKinds[k].name, uniform[k].instrs.size());
        std::printf("  %-18s %zu\n  %-18s %zu\n", "cyclic", ni, "random",
                    random_t.instrs.size());
        return 1;
    }

    std::vector<Arm> arms;
    for (std::size_t k = 0; k < nkinds; ++k) arms.push_back({kKinds[k].name, &uniform[k], {}, {}});
    arms.push_back({"MIXED cyclic", &cyclic, {}, {}});
    arms.push_back({"MIXED blocked", &blocked, {}, {}});
    arms.push_back({"MIXED random", &random_t, {}, {}});

    std::vector<float> out(count);
    // Warm up every arm once before any timing, so no arm pays first-touch.
    for (Arm& a : arms) {
        time_eval(*a.tape, pts, out);
        time_dispatched(*a.tape, pts, out);
    }

    std::vector<double> disp_ms, dir_ms;
    std::vector<float> ref(count), r_disp(count), r_dir(count);
    time_eval(uniform[0], pts, ref);

    // INTERLEAVED: one round touches every arm before any arm gets a second.
    for (int r = 0; r < rounds; ++r) {
        for (Arm& a : arms) {
            a.ms.push_back(time_eval(*a.tape, pts, out));
            a.lean.push_back(time_dispatched(*a.tape, pts, out));
        }
        disp_ms.push_back(time_dispatched(uniform[0], pts, r_disp));
        dir_ms.push_back(time_direct(uniform[0], pts, r_dir));
    }

    double worst = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(r_disp[i] - ref[i])));
        worst = std::max(worst, std::fabs(static_cast<double>(r_dir[i] - ref[i])));
    }

    const double per_instr = 1e6 / (static_cast<double>(count) * static_cast<double>(ni));
    std::printf("items=%zu kinds=%zu instrs=%zu points=%zu rounds=%d seed=%llu\n", items, nkinds,
                ni, count, rounds, static_cast<unsigned long long>(seed));
    std::printf("DISPATCHED/DIRECT deviation from ctape_eval: %.3g%s\n\n", worst,
                worst == 0.0 ? "  (bit-identical)" : "  <-- NOT the same maths");

    double usum = 0.0, lsum = 0.0;
    std::printf("%-20s %9s %13s %6s | %9s %13s %6s\n", "arm", "V0 ns/i", "range", "cv%",
                "lean ns/i", "range", "cv%");
    for (std::size_t k = 0; k < arms.size(); ++k) {
        const Arm& a = arms[k];
        const double med = median(a.ms) * per_instr;
        const double lo = *std::min_element(a.ms.begin(), a.ms.end()) * per_instr;
        const double hi = *std::max_element(a.ms.begin(), a.ms.end()) * per_instr;
        const double lmed = median(a.lean) * per_instr;
        const double llo = *std::min_element(a.lean.begin(), a.lean.end()) * per_instr;
        const double lhi = *std::max_element(a.lean.begin(), a.lean.end()) * per_instr;
        if (k < nkinds) { usum += med; lsum += lmed; }
        const bool bad = cv_pct(a.ms) > 10.0 || cv_pct(a.lean) > 10.0;
        std::printf("%-20s %9.3f %6.2f-%6.2f %5.1f | %9.3f %6.2f-%6.2f %5.1f%s\n", a.name, med,
                    lo, hi, cv_pct(a.ms), lmed, llo, lhi, cv_pct(a.lean),
                    bad ? "  <-- UNUSABLE" : "");
    }
    const double umean = usum / static_cast<double>(nkinds);
    const double lmean = lsum / static_cast<double>(nkinds);
    std::printf("%-20s %9.3f %26s %9.3f\n", "mean(uniform)", umean, "|", lmean);

    const double cyc = median(arms[nkinds].ms) * per_instr;
    const double blk = median(arms[nkinds + 1].ms) * per_instr;
    const double rnd = median(arms[nkinds + 2].ms) * per_instr;
    const double lcyc = median(arms[nkinds].lean) * per_instr;
    const double lblk = median(arms[nkinds + 1].lean) * per_instr;
    const double lrnd = median(arms[nkinds + 2].lean) * per_instr;
    std::printf("\n%-38s %8s %8s\n", "", "V0", "lean");
    std::printf("%-38s %7.3fx %7.3fx   mixed footprint, order learnable\n",
                "cyclic  / mean(uniform)", cyc / umean, lcyc / lmean);
    std::printf("%-38s %7.3fx %7.3fx   CONTROL: predictable order, scrambled pairing\n",
                "blocked / cyclic", blk / cyc, lblk / lcyc);
    std::printf("%-38s %7.3fx %7.3fx   opcode order alone: misprediction\n",
                "random  / cyclic", rnd / cyc, lrnd / lcyc);
    std::printf("%-38s %7.3fx %7.3fx   whole mixed-vs-uniform cost\n",
                "random  / mean(uniform)", rnd / umean, lrnd / lmean);

    const double d1 = median(disp_ms) * per_instr, d2 = median(dir_ms) * per_instr;
    std::printf("\nstripped loop, uniform sphere tape (bounds dispatch+call+prologue):\n");
    std::printf("  DISPATCHED via ctape_prim_dist %8.3f ns/instr  cv %.1f%%\n", d1,
                cv_pct(disp_ms));
    std::printf("  DIRECT     sd_sphere inline    %8.3f ns/instr  cv %.1f%%\n", d2,
                cv_pct(dir_ms));
    std::printf("  ratio                          %8.3fx\n", d1 / d2);
    return worst == 0.0 ? 0 : 1;
}
