// The measurement behind issue #207 candidate 4, kept so its table can be
// re-derived rather than trusted. NOT a gated benchmark.
//
// The claim under test: "the stack machine -- `CTapeValue stack[CLAY_TAPE_MAX_STACK]`
// per call, pushed and popped per instruction". Two different stacks now answer
// to that name and they have to be sized separately:
//
//   SCALAR  `ctape_eval` (include/clay/kernel/tape.h): a 16-slot CTapeValue
//           array in the frame, indexed by a runtime `top`. 16 bytes a slot,
//           and a dynamic index means the array cannot be promoted to
//           registers -- every push is a store and every pop is a load.
//           This is the path `mask_extrude` drives (#225).
//
//   BLOCKED `walk_blocked` (backends/cpu/tape_block.cpp): `block * depth` slots
//           in a thread-local vector, so a slot is an ARRAY across the block
//           rather than a register, and #223 already narrowed it to one float
//           per slot for a distance-only query. Its traffic is a real working
//           set, not a spill.
//
// THE DESIGN. One tape, one set of points, and arms that differ ONLY in where a
// stack slot lives. Everything else -- the opcode dispatch, the header loads,
// the transform, the primitive, the combine, the branch set -- is shared code
// called from every arm, so a difference between two arms is storage and
// nothing else. Every arm is asserted BIT-IDENTICAL to `ctape_eval`.
//
//   SCALAR arms, coloured (`CTapeValue`) and distance-only (`float`):
//     arr   `Slot stack[CLAY_TAPE_MAX_STACK]`, runtime `top`  <- what ships
//     sel   two named locals + `top` selects, same loop, same dispatch.
//           The realistic alternative: a tape whose depth the compiler already
//           computes (`tape_stack_depth`) could emit this. Isolates MEMORY
//           TRAFFIC from register-plus-select.
//     pair  the tape pre-scanned once into (prim, combine) pairs; the walk is
//           an accumulator and a value, no array and no opcode dispatch.
//           THE CEILING, and it is a ceiling for candidate 3 as well as 4 --
//           it deletes the dispatch branch too, so `pair` is an upper bound on
//           the pair of them and not an estimate of the stack alone.
//
//   BLOCKED arms, coloured and distance-only:
//     arr   staged through the slot array exactly as `walk_blocked` does: the
//           prim loop stores n values, the combine loop loads two and stores
//           one.
//     pair  prim and combine FUSED into one pass -- the prim value stays in a
//           register and is combined immediately, so slot `top` is never
//           written or read. Half the slot traffic and half the passes over
//           the block. The realistic alternative here; a register machine is
//           not available because a blocked slot is n points wide.
//
//   The shipping `eval_points_blocked` is timed alongside as the control that
//   says the probe's `arr` is a faithful transcription rather than a strawman.
//
// WHAT IT CANNOT SEPARATE. Store-to-load forwarding latency from cache traffic
// in the scalar arms -- a 2-slot working set is L1-resident either way, so what
// `arr - sel` charges is the forwarding and the address arithmetic, not misses.
// And `pair` folds the dispatch branch in, deliberately, as noted above.
//
//   ./tape_stack_probe [items] [points] [rounds] [prim-kind] [block]
//
// prim-kind indexes the table below and exists to answer the dilution question:
// stack traffic is a FIXED cost per instruction, so its fraction of the whole
// falls as the primitive gets dearer.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "clay/eval/backend.h"
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

// Cheap to dear, so the dilution of a fixed per-instruction cost is visible.
// Pure-analytic only: no blob, no deformer, no repeat.
struct PrimKind {
    const char* name;
    scene::Prim (*make)();
};
const PrimKind kKinds[] = {
    {"sphere", [] { return scene::Prim::sphere(0.12f); }},
    {"box", [] { return scene::Prim::box(cf3(0.10f, 0.11f, 0.12f)); }},
    {"torus", [] { return scene::Prim::torus(0.09f, 0.03f); }},
    {"octahedron", [] { return scene::Prim::octahedron(0.12f); }},
    {"lnorm_sphere", [] { return scene::Prim::lnorm_sphere(0.12f, 4.0f); }},
};
constexpr std::size_t kNumKinds = sizeof(kKinds) / sizeof(kKinds[0]);

scene::Document make_document(std::size_t items, std::size_t kind) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("probe");
    std::uint64_t s = 4242;
    for (std::size_t i = 0; i < items; ++i) {
        scene::Node n;
        n.prim = kKinds[kind].make();
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

// ---------------------------------------------------------------------------
// The shared bodies. Every arm below calls THESE, so no arm can differ from
// another in the arithmetic, the dispatch or the branch set -- only in where it
// puts the result.
// ---------------------------------------------------------------------------

// The prim branch of `ctape_eval`, verbatim, minus nothing.
inline CTapeValue prim_step(CLAY_UINT_T op, const float* pr, const float* blob, cfloat3 p) {
    cfloat4x4 inv;
    inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
    inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
    inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
    inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
    CTapeValue v;
    v.color = cf3(pr[14], pr[15], pr[16]);
    const cfloat3 lp = cmul_point(inv, p);
    const float* repeat = pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS;
    float prim_value;
    cfloat3 repeat_color = v.color;
    if (!ctape_repeat_active(repeat)) {
        prim_value = ctape_prim_local(op, pr, blob, lp, CLAY_OUTARG(v.color));
    } else if (ctape_repeat_is_radial(repeat)) {
        const float d0 =
            ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0),
                             CLAY_OUTARG(repeat_color));
        const int neighbour = crep_radial_neighbor(lp, CLAY_INT(CLAY_AT(repeat, 1)));
        const float d1 = ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, neighbour),
                                          CLAY_OUTARG(repeat_color));
        prim_value = cmin(d0, d1);
    } else {
        prim_value = ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0),
                                      CLAY_OUTARG(repeat_color));
    }
    v.d = prim_value * pr[12] - pr[13];
    return v;
}

// The combine branch of `ctape_eval`, verbatim: transition, feather, plain, and
// the gate that composes with all three.
inline CTapeValue combine_step(CTapeValue a, CTapeValue b, const float* pr, const float* blob,
                               cfloat3 p) {
    const int mode = CLAY_INT(pr[0]);
    CTapeValue combined;
    if (ctape_mode_is_transition(mode)) {
        const float w = ctape_transition_weight(mode, pr + CLAY_TAPE_COMBINE_HEADER, p);
        combined.d = cmix(a.d, b.d, w);
        combined.color = cmix(a.color, b.color, w);
    } else if (mode == ccombine_replace_feather) {
        combined = ctape_replace_feather(a, b, pr + CLAY_TAPE_COMBINE_HEADER, blob, p);
    } else {
        combined = ctape_combine_values(a, b, mode, CLAY_INT(pr[1]), pr[2], pr[3]);
    }
    const int gate_off = CLAY_INT(pr[4]);
    if (gate_off >= 0) {
        const float g = ctape_gate_weight(blob + gate_off, blob, p);
        if (g >= 1.0f) {
            combined = a;
        } else {
            combined.d = cmix(combined.d, a.d, g);
            combined.color = cmix(combined.color, a.color, g);
        }
    }
    return combined;
}

// The distance-only halves, which is what the blocked evaluator's `plain` loop
// runs after #223. No colour anywhere, so a slot is one float.
inline float prim_step_d(CLAY_UINT_T op, const float* pr, const float* blob, cfloat3 p) {
    cfloat4x4 inv;
    inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
    inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
    inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
    inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
    return (ctape_prim_dist(op, pr + CLAY_TAPE_PRIM_HEADER, blob, cmul_point(inv, p)) + 0.0f) *
               pr[12] -
           pr[13];
}
inline float combine_step_d(float a, float b, const float* pr) {
    return ctape_combine_dist(a, b, CLAY_INT(pr[0]), CLAY_INT(pr[1]), pr[2], pr[3]);
}

// ---------------------------------------------------------------------------
// SCALAR arms. `arr` is the shipped shape; `sel` and `pair` are the two
// alternatives. Identical loops apart from the three lines that touch a slot.
// ---------------------------------------------------------------------------

struct Pair {  // pre-scanned: the prim to evaluate, and the combine to fold it
    CLAY_UINT_T op;
    const float* prim;
    const float* comb;  // nullptr for the seed
};

[[gnu::noinline]] double time_scalar_arr(const scene::Tape& t, const std::vector<float>& pts,
                       std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const float* bl = t.blob.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        CTapeValue stack[CLAY_TAPE_MAX_STACK];
        int top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                if (top < 1) continue;
                const CTapeValue b = stack[top - 1];
                CTapeValue a;
                a.d = CLAY_TAPE_FAR;
                a.color = b.color;
                if (top >= 2) {
                    a = stack[top - 2];
                    --top;
                }
                stack[top - 1] = combine_step(a, b, pr, bl, p);
            } else {
                if (top >= CLAY_TAPE_MAX_STACK) continue;
                stack[top] = prim_step(in[k].op, pr, bl, p);
                ++top;
            }
        }
        out[i] = stack[top - 1].d;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Two named locals in place of the array. Valid only for depth <= 2, which the
// caller checks with `tape_stack_depth`; the selects are the price a register
// machine pays for a `top` that is still a runtime value.
[[gnu::noinline]] double time_scalar_sel(const scene::Tape& t, const std::vector<float>& pts,
                       std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const float* bl = t.blob.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        CTapeValue s0, s1;
        int top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                if (top < 1) continue;
                const CTapeValue b = (top >= 2) ? s1 : s0;
                CTapeValue a;
                a.d = CLAY_TAPE_FAR;
                a.color = b.color;
                if (top >= 2) {
                    a = s0;
                    --top;
                }
                const CTapeValue r = combine_step(a, b, pr, bl, p);
                if (top >= 2) s1 = r; else s0 = r;
            } else {
                if (top >= 2) continue;
                const CTapeValue v = prim_step(in[k].op, pr, bl, p);
                if (top == 0) s0 = v; else s1 = v;
                ++top;
            }
        }
        out[i] = ((top >= 2) ? s1 : s0).d;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// No stack and no opcode dispatch: the tape pre-scanned once into pairs and
// walked as an accumulator. The ceiling for candidates 3 and 4 together.
[[gnu::noinline]] double time_scalar_pair(const std::vector<Pair>& prog, const float* bl,
                        const std::vector<float>& pts, std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const Pair* pp = prog.data();
    const std::size_t np = prog.size();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        CTapeValue acc = prim_step(pp[0].op, pp[0].prim, bl, p);
        for (std::size_t k = 1; k < np; ++k) {
            const CTapeValue v = prim_step(pp[k].op, pp[k].prim, bl, p);
            acc = combine_step(acc, v, pp[k].comb, bl, p);
        }
        out[i] = acc.d;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The same three, distance-only: one float a slot.
[[gnu::noinline]] double time_scalar_arr_d(const scene::Tape& t, const std::vector<float>& pts,
                         std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const float* bl = t.blob.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float stack[CLAY_TAPE_MAX_STACK];
        int top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                if (top < 1) continue;
                const float b = stack[top - 1];
                float a = CLAY_TAPE_FAR;
                if (top >= 2) {
                    a = stack[top - 2];
                    --top;
                }
                stack[top - 1] = combine_step_d(a, b, pr);
            } else {
                if (top >= CLAY_TAPE_MAX_STACK) continue;
                stack[top] = prim_step_d(in[k].op, pr, bl, p);
                ++top;
            }
        }
        out[i] = stack[top - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

[[gnu::noinline]] double time_scalar_sel_d(const scene::Tape& t, const std::vector<float>& pts,
                         std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const float* bl = t.blob.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float s0 = 0.0f, s1 = 0.0f;
        int top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                if (top < 1) continue;
                const float b = (top >= 2) ? s1 : s0;
                float a = CLAY_TAPE_FAR;
                if (top >= 2) {
                    a = s0;
                    --top;
                }
                const float r = combine_step_d(a, b, pr);
                if (top >= 2) s1 = r; else s0 = r;
            } else {
                if (top >= 2) continue;
                const float v = prim_step_d(in[k].op, pr, bl, p);
                if (top == 0) s0 = v; else s1 = v;
                ++top;
            }
        }
        out[i] = (top >= 2) ? s1 : s0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

[[gnu::noinline]] double time_scalar_pair_d(const std::vector<Pair>& prog, const float* bl,
                          const std::vector<float>& pts, std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const Pair* pp = prog.data();
    const std::size_t np = prog.size();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float acc = prim_step_d(pp[0].op, pp[0].prim, bl, p);
        for (std::size_t k = 1; k < np; ++k)
            acc = combine_step_d(acc, prim_step_d(pp[k].op, pp[k].prim, bl, p), pp[k].comb);
        out[i] = acc;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The same arr/pair pair with the primitive INLINED -- `sd_sphere` directly,
// no opcode read and no out-of-line call. The scalar arms above all cross a
// `bl ctape_prim_dist` between a push and its pop (AppleClang emits that call
// in the shipping walk too), and a value held in a register across a call has
// to be callee-saved, which is exactly the constraint being tested. These two
// remove the call from BOTH sides, so they measure the stack against a register
// in the regime that most favours the register. Sphere tapes only.
[[gnu::noinline]] double time_scalar_arr_direct(const scene::Tape& t,
                                                const std::vector<float>& pts,
                                                std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const CTapeInstr* in = t.instrs.data();
    const std::size_t ni = t.instrs.size();
    const float* pa = t.params.data();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float stack[CLAY_TAPE_MAX_STACK];
        int top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const float* pr = pa + in[k].param_offset;
            if (in[k].op == ctape_combine) {
                const float b = stack[top - 1];
                const float a = stack[top - 2];
                --top;
                stack[top - 1] = ctape_smin_m(CLAY_INT(pr[1]), a, b, pr[2]).x;
            } else {
                cfloat4x4 inv;
                inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
                inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
                inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
                inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
                stack[top++] =
                    sd_sphere(cmul_point(inv, p), pr[CLAY_TAPE_PRIM_HEADER]) * pr[12] - pr[13];
            }
        }
        out[i] = stack[top - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

[[gnu::noinline]] double time_scalar_pair_direct(const std::vector<Pair>& prog,
                                                 const std::vector<float>& pts,
                                                 std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const Pair* pp = prog.data();
    const std::size_t np = prog.size();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        float acc = 0.0f;
        for (std::size_t k = 0; k < np; ++k) {
            const float* pr = pp[k].prim;
            cfloat4x4 inv;
            inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
            inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
            inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
            inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
            const float d =
                sd_sphere(cmul_point(inv, p), pr[CLAY_TAPE_PRIM_HEADER]) * pr[12] - pr[13];
            if (k == 0) {
                acc = d;
            } else {
                const float* cp = pp[k].comb;
                acc = ctape_smin_m(CLAY_INT(cp[1]), acc, d, cp[2]).x;
            }
        }
        out[i] = acc;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
// BLOCKED arms. A slot here is `block` values wide, so there is no register
// machine to compare against -- only staged (what ships) against fused.
// ---------------------------------------------------------------------------

// The per-block decode `walk_blocked` hoists out of the point loop. Without it
// these arms would measure the scalar decode instead, and the shipped-blocked
// control below says so loudly when it is missing.
struct PrimH {
    cfloat4x4 inv;
    float scale, round;
    cfloat3 color;
};
PrimH decode_prim_h(const float* pr) {
    PrimH h;
    h.inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
    h.inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
    h.inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
    h.inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
    h.scale = pr[12];
    h.round = pr[13];
    h.color = cf3(pr[14], pr[15], pr[16]);
    return h;
}
struct CombH {
    int mode, profile;
    float k, rb;
};
CombH decode_comb_h(const float* pr) {
    CombH c;
    c.mode = CLAY_INT(pr[0]);
    c.profile = CLAY_INT(pr[1]);
    c.k = pr[2];
    c.rb = pr[3];
    return c;
}
// `walk_blocked`'s fast prim arm, verbatim: no repeat, no deformer, not a
// volume, so `ctape_prim_dist` directly and the `+ 0.0f` the general path adds.
inline float blk_prim_d(const PrimH& h, CLAY_UINT_T op, const float* pr, const float* blob,
                        cfloat3 p) {
    return (ctape_prim_dist(op, pr + CLAY_TAPE_PRIM_HEADER, blob, cmul_point(h.inv, p)) + 0.0f) *
               h.scale -
           h.round;
}

// Mode 0 STAGED  two passes, prim's value parked in slot 1  <- what ships
// Mode 1 FUSED   one pass, prim's value stays in a register
// Mode 2 ONEPASS one pass, but still written to slot 1 and read straight back.
//                The decomposition: 0 -> 2 is the second PASS over the block,
//                2 -> 1 is the slot TRAFFIC. Without it the fused arm's win is
//                two things at once and neither is sized.
template <bool Coloured, int Mode>
[[gnu::noinline]] double time_blocked(const std::vector<Pair>& prog, const scene::Tape& t,
                                      const std::vector<float>& pts, std::size_t block,
                                      std::vector<float>& out, std::vector<float>& cout) {
    using Slot = typename std::conditional<Coloured, CTapeValue, float>::type;
    const std::size_t count = pts.size() / 3;
    const float* bl = t.blob.data();
    const Pair* pp = prog.data();
    const std::size_t np = prog.size();
    const std::size_t span = std::min(block, count);
    static thread_local std::vector<Slot> stack;
    if (stack.size() < span * 2) stack.resize(span * 2);
    Slot* s0 = stack.data();
    Slot* s1 = stack.data() + span;

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t base = 0; base < count; base += span) {
        const std::size_t n = std::min(span, count - base);
        const auto point = [&](std::size_t j) {
            const std::size_t i = base + j;
            return cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        };
        {
            const PrimH h = decode_prim_h(pp[0].prim);
            for (std::size_t j = 0; j < n; ++j) {
                const float d = blk_prim_d(h, pp[0].op, pp[0].prim, bl, point(j));
                if constexpr (Coloured) {
                    s0[j].color = h.color;
                    s0[j].d = d;
                } else {
                    s0[j] = d;
                }
            }
        }
        for (std::size_t k = 1; k < np; ++k) {
            const PrimH h = decode_prim_h(pp[k].prim);  // once per block
            const CombH c = decode_comb_h(pp[k].comb);  // once per block
            if constexpr (Mode == 1) {
                // One pass. The prim value never reaches a slot.
                for (std::size_t j = 0; j < n; ++j) {
                    const float d = blk_prim_d(h, pp[k].op, pp[k].prim, bl, point(j));
                    if constexpr (Coloured) {
                        CTapeValue v;
                        v.color = h.color;
                        v.d = d;
                        s0[j] = ctape_combine_values(s0[j], v, c.mode, c.profile, c.k, c.rb);
                    } else {
                        s0[j] = ctape_combine_dist(s0[j], d, c.mode, c.profile, c.k, c.rb);
                    }
                }
            } else if constexpr (Mode == 2) {
                // One pass, but the prim's value still goes to slot 1 and comes
                // straight back out. Same traffic as STAGED, one fewer pass.
                for (std::size_t j = 0; j < n; ++j) {
                    const float d = blk_prim_d(h, pp[k].op, pp[k].prim, bl, point(j));
                    if constexpr (Coloured) {
                        s1[j].color = h.color;
                        s1[j].d = d;
                        s0[j] = ctape_combine_values(s0[j], s1[j], c.mode, c.profile, c.k, c.rb);
                    } else {
                        s1[j] = d;
                        s0[j] = ctape_combine_dist(s0[j], s1[j], c.mode, c.profile, c.k, c.rb);
                    }
                }
            } else {
                // Two passes, staged through slot 1 -- what `walk_blocked` does.
                for (std::size_t j = 0; j < n; ++j) {
                    const float d = blk_prim_d(h, pp[k].op, pp[k].prim, bl, point(j));
                    if constexpr (Coloured) {
                        s1[j].color = h.color;
                        s1[j].d = d;
                    } else {
                        s1[j] = d;
                    }
                }
                for (std::size_t j = 0; j < n; ++j) {
                    if constexpr (Coloured)
                        s0[j] = ctape_combine_values(s0[j], s1[j], c.mode, c.profile, c.k, c.rb);
                    else
                        s0[j] = ctape_combine_dist(s0[j], s1[j], c.mode, c.profile, c.k, c.rb);
                }
            }
        }
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = base + j;
            if constexpr (Coloured) {
                out[i] = s0[j].d;
                cout[i * 3] = s0[j].color.x;
                cout[i * 3 + 1] = s0[j].color.y;
                cout[i * 3 + 2] = s0[j].color.z;
            } else {
                out[i] = s0[j];
            }
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}


[[gnu::noinline]] double time_shipped_blocked(const scene::Tape& t, const std::vector<float>& pts, bool coloured,
                            std::size_t block, std::vector<float>& out,
                            std::vector<float>& cout) {
    eval::PointQuery q{pts.data(), pts.size() / 3, 1e-4f};
    eval::PointResults r;
    r.distances = out.data();
    r.colors_rgb = coloured ? cout.data() : nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    eval::eval_points_blocked(t, q, r, block);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

[[gnu::noinline]] double time_shipped_scalar(const scene::Tape& t, const std::vector<float>& pts,
                           std::vector<float>& out) {
    const std::size_t count = pts.size() / 3;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i)
        out[i] = t.eval(cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2])).d;
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

struct Arm {
    const char* name;
    std::vector<double> ms;
    std::vector<float>* result;  // checked bit-identical against ctape_eval
};

}  // namespace

int main(int argc, char** argv) {
    const std::size_t items = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 256;
    const std::size_t count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 20480;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 9;
    const std::size_t kind = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 0;
    const std::size_t block = argc > 5 ? std::strtoul(argv[5], nullptr, 10) : 512;
    if (kind >= kNumKinds) {
        std::printf("prim-kind must be < %zu\n", kNumKinds);
        return 1;
    }

    const std::vector<float> pts = make_points(count);
    const scene::Tape tape = compile_document(make_document(items, kind));
    const std::size_t ni = tape.instrs.size();
    const std::size_t depth = eval::tape_stack_depth(tape);

    // Pre-scan into (prim, combine) pairs. Refuse anything that is not the flat
    // accumulator chain the `pair` and blocked-fused arms assume -- they would
    // silently compute something else otherwise, and the bit-identity check
    // below is the backstop rather than the guard.
    std::vector<Pair> prog;
    {
        bool ok = depth <= 2 && ni >= 1 && tape.instrs[0].op != ctape_combine;
        for (std::size_t k = 0; ok && k < ni;) {
            if (tape.instrs[k].op == ctape_combine) { ok = false; break; }
            Pair pr;
            pr.op = tape.instrs[k].op;
            pr.prim = tape.params.data() + tape.instrs[k].param_offset;
            pr.comb = nullptr;
            if (k + 1 < ni) {
                if (prog.empty()) {
                    // seed: the first prim is pushed and not yet combined
                    prog.push_back(pr);
                    ++k;
                    continue;
                }
                if (tape.instrs[k + 1].op != ctape_combine) { ok = false; break; }
                pr.comb = tape.params.data() + tape.instrs[k + 1].param_offset;
                prog.push_back(pr);
                k += 2;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok || prog.size() < 2) {
            std::printf("tape is not a flat accumulator chain (ni=%zu depth=%zu); refusing\n", ni,
                        depth);
            return 1;
        }
    }

    std::vector<float> ref(count), cref(count * 3);
    std::vector<float> r_sa(count), r_ss(count), r_sp(count);
    std::vector<float> r_da(count), r_ds(count), r_dp(count);
    std::vector<float> r_ba(count), r_bp(count), r_bo(count), r_bca(count), r_bcp(count),
        r_bco(count), r_bs(count);
    std::vector<float> r_ad(count), r_pd(count);
    std::vector<float> cscratch(count * 3);

    time_shipped_scalar(tape, pts, ref);

    std::vector<Arm> arms = {
        {"SCALAR coloured arr  (SHIPPED)", {}, &r_sa},
        {"SCALAR coloured sel", {}, &r_ss},
        {"SCALAR coloured pair", {}, &r_sp},
        {"SCALAR dist arr", {}, &r_da},
        {"SCALAR dist sel", {}, &r_ds},
        {"SCALAR dist pair", {}, &r_dp},
        {"BLOCKED dist arr  (SHIPPED)", {}, &r_ba},
        {"BLOCKED dist onepass-slot", {}, &r_bo},
        {"BLOCKED dist pair", {}, &r_bp},
        {"BLOCKED coloured arr  (SHIPPED)", {}, &r_bca},
        {"BLOCKED coloured onepass-slot", {}, &r_bco},
        {"BLOCKED coloured pair", {}, &r_bcp},
    };
    // Sphere only: the inlined-primitive pair, which has no `bl` between a push
    // and its pop.
    const bool direct = kind == 0;
    if (direct) {
        arms.push_back({"SCALAR dist arr  (inlined prim)", {}, &r_ad});
        arms.push_back({"SCALAR dist pair (inlined prim)", {}, &r_pd});
    }
    std::vector<double> ship_scalar, ship_blk_d, ship_blk_c;

    const float* bl = tape.blob.data();
    auto round_once = [&](bool record) {
        double v;
        v = time_scalar_arr(tape, pts, r_sa);           if (record) arms[0].ms.push_back(v);
        v = time_scalar_sel(tape, pts, r_ss);           if (record) arms[1].ms.push_back(v);
        v = time_scalar_pair(prog, bl, pts, r_sp);      if (record) arms[2].ms.push_back(v);
        v = time_scalar_arr_d(tape, pts, r_da);         if (record) arms[3].ms.push_back(v);
        v = time_scalar_sel_d(tape, pts, r_ds);         if (record) arms[4].ms.push_back(v);
        v = time_scalar_pair_d(prog, bl, pts, r_dp);    if (record) arms[5].ms.push_back(v);
        v = time_blocked<false, 0>(prog, tape, pts, block, r_ba, cscratch);
        if (record) arms[6].ms.push_back(v);
        v = time_blocked<false, 2>(prog, tape, pts, block, r_bo, cscratch);
        if (record) arms[7].ms.push_back(v);
        v = time_blocked<false, 1>(prog, tape, pts, block, r_bp, cscratch);
        if (record) arms[8].ms.push_back(v);
        v = time_blocked<true, 0>(prog, tape, pts, block, r_bca, cscratch);
        if (record) arms[9].ms.push_back(v);
        v = time_blocked<true, 2>(prog, tape, pts, block, r_bco, cscratch);
        if (record) arms[10].ms.push_back(v);
        v = time_blocked<true, 1>(prog, tape, pts, block, r_bcp, cscratch);
        if (record) arms[11].ms.push_back(v);
        if (direct) {
            v = time_scalar_arr_direct(tape, pts, r_ad);  if (record) arms[12].ms.push_back(v);
            v = time_scalar_pair_direct(prog, pts, r_pd); if (record) arms[13].ms.push_back(v);
        }
        v = time_shipped_scalar(tape, pts, r_bs);       if (record) ship_scalar.push_back(v);
        v = time_shipped_blocked(tape, pts, false, block, r_bs, cscratch);
        if (record) ship_blk_d.push_back(v);
        v = time_shipped_blocked(tape, pts, true, block, r_bs, cref);
        if (record) ship_blk_c.push_back(v);
    };

    round_once(false);  // warm every arm before any arm is timed
    for (int r = 0; r < rounds; ++r) round_once(true);

    double worst = 0.0;
    for (const Arm& a : arms)
        for (std::size_t i = 0; i < count; ++i)
            worst = std::max(worst, std::fabs(static_cast<double>((*a.result)[i] - ref[i])));

    const double per_instr = 1e6 / (static_cast<double>(count) * static_cast<double>(ni));
    std::printf("prim=%s items=%zu instrs=%zu depth=%zu points=%zu block=%zu rounds=%d\n",
                kKinds[kind].name, items, ni, depth, count, block, rounds);
    std::printf("all arms vs ctape_eval: worst deviation %.3g%s\n\n", worst,
                worst == 0.0 ? "  (bit-identical)" : "  <-- NOT the same maths");

    std::printf("%-32s %9s %15s %6s\n", "arm", "ns/instr", "range", "cv%");
    std::vector<double> med(arms.size());
    for (std::size_t k = 0; k < arms.size(); ++k) {
        const Arm& a = arms[k];
        med[k] = median(a.ms) * per_instr;
        const double lo = *std::min_element(a.ms.begin(), a.ms.end()) * per_instr;
        const double hi = *std::max_element(a.ms.begin(), a.ms.end()) * per_instr;
        const double cv = cv_pct(a.ms);
        std::printf("%-32s %9.3f %6.3f-%7.3f %5.1f%s\n", a.name, med[k], lo, hi, cv,
                    cv > 10.0 ? "  <-- UNUSABLE" : "");
    }
    auto ctl = [&](const char* n, std::vector<double>& v) {
        std::printf("%-32s %9.3f %6.3f-%7.3f %5.1f   CONTROL\n", n, median(v) * per_instr,
                    *std::min_element(v.begin(), v.end()) * per_instr,
                    *std::max_element(v.begin(), v.end()) * per_instr, cv_pct(v));
    };
    ctl("ctape_eval (scalar, coloured)", ship_scalar);
    ctl("eval_points_blocked dist", ship_blk_d);
    ctl("eval_points_blocked coloured", ship_blk_c);

    std::printf("\n%-40s %8s %10s\n", "", "ratio", "ns/instr");
    auto row = [&](const char* n, double a, double b) {
        std::printf("%-40s %7.3fx %+9.3f\n", n, a / b, a - b);
    };
    row("SCALAR coloured arr / sel", med[0], med[1]);
    row("SCALAR coloured arr / pair", med[0], med[2]);
    row("SCALAR dist     arr / sel", med[3], med[4]);
    row("SCALAR dist     arr / pair", med[3], med[5]);
    row("BLOCKED dist     arr / onepass  (the PASS)", med[6], med[7]);
    row("BLOCKED dist     onepass / pair (the TRAFFIC)", med[7], med[8]);
    row("BLOCKED dist     arr / pair", med[6], med[8]);
    row("BLOCKED coloured arr / onepass  (the PASS)", med[9], med[10]);
    row("BLOCKED coloured onepass / pair (the TRAFFIC)", med[10], med[11]);
    row("BLOCKED coloured arr / pair", med[9], med[11]);
    if (direct) row("SCALAR inlined-prim arr / pair", med[12], med[13]);
    std::printf("\ntranscription controls (should be ~1.00x):\n");
    row("ctape_eval / SCALAR coloured arr", median(ship_scalar) * per_instr, med[0]);
    row("eval_points_blocked d / BLOCKED dist arr", median(ship_blk_d) * per_instr, med[6]);
    row("eval_points_blocked c / BLOCKED col arr", median(ship_blk_c) * per_instr, med[9]);
    return worst == 0.0 ? 0 : 1;
}
