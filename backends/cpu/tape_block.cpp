// The blocked tape evaluator: one walk of the instruction sequence per block of
// points, against `ctape_eval`'s one walk per point.
//
// This file MIRRORS `ctape_eval` (include/clay/kernel/tape.h) branch for branch.
// It is a second implementation of the same walk, which is a real hazard — two
// interpreters that must agree forever — and the whole defence is that they are
// asserted BIT-IDENTICAL rather than merely close. When `ctape_eval` changes,
// this changes with it, and `test_tape_block.cpp` is what catches the case where
// it did not.
//
// What is hoisted is only the DECODE: the 17-float prim header and the assembled
// inverse transform, a combine's mode, profile, blend constants and gate offset.
// Every value that depends on the point stays inside the point loop, and the
// arithmetic applied to each point is the same expression in the same order, so
// identity is a property of the structure rather than of a tolerance.
//
// Control flow is uniform across a block. Every branch in `ctape_eval` selects
// on the opcode or on the stack depth — never on a point's value — so all points
// in a block take the same path and one shared `top` is correct for all of them.

#include <algorithm>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/kernel/tape.h"

namespace clay {
namespace eval {

using kernel::cf3;
using kernel::cf4;
using kernel::cfloat3;
using kernel::cfloat4x4;
using kernel::CTapeValue;

namespace {

// A brick is 8^3 = 512 points, which is the sculpting path's natural unit and
// already sits in the flat part of the block-size curve. Everything from 64 up
// measured within 4%, so this is a default rather than a tuned constant.
constexpr std::size_t kDefaultBlock = 512;

// One prim instruction's parameter header, decoded once per block.
struct PrimHeader {
    cfloat4x4 inv;
    float scale;
    float round;
    cfloat3 color;
    const float* repeat;
    bool repeat_active;
    bool repeat_radial;
    // Whether this instruction uses the features `ctape_prim_local` tests for
    // on every call. Both are properties of the INSTRUCTION, not of the point,
    // so a blocked walk decides them once per block — which is the whole reason
    // blocking helps beyond the header load. The prototype measured these
    // absent-feature checks at 1.6x on a tape that uses none of them.
    bool has_deformers;
    bool is_volume;
};

PrimHeader decode_prim(const float* pr, CLAY_UINT_T op) {
    PrimHeader h;
    h.is_volume = (op == kernel::ctape_volume);
    h.inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
    h.inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
    h.inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
    h.inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
    h.scale = pr[12];
    h.round = pr[13];
    h.color = cf3(pr[14], pr[15], pr[16]);
    h.repeat = pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS;
    h.repeat_active = kernel::ctape_repeat_active(h.repeat);
    h.repeat_radial = kernel::ctape_repeat_is_radial(h.repeat);
    const float* deform = pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS +
                          CLAY_TAPE_REPEAT_FLOATS;
    h.has_deformers = CLAY_INT(deform[0]) != 0;
    return h;
}

// The prim branch's per-point half, transcribed from `ctape_eval`. `v.color`
// enters holding the item's colour and a sampled volume may overwrite it.
CTapeValue prim_at(CLAY_UINT_T op, const float* pr, const float* blob,
                   const PrimHeader& h, cfloat3 p) {
    CTapeValue v;
    v.color = h.color;
    const cfloat3 lp = kernel::cmul_point(h.inv, p);
    float value;
    // A REPEATED volume reports the item's colour rather than its samples',
    // exactly as the scalar path does: the repeat paths evaluate more than once
    // and take a min, so a colour written by whichever call ran last would not
    // be the colour of the instance that won.
    cfloat3 repeat_color = v.color;
    if (!h.repeat_active) {
        // The common instruction: no repeat, no deformer chain, not a sampled
        // volume. `ctape_prim_local` would re-derive all three per point. The
        // `+ 0.0f` is the deformer offset it would have added and is KEPT
        // rather than folded away: dropping it turns a -0.0f result into +0.0f,
        // which is a different bit pattern and the identity test says so.
        if (!h.has_deformers && !h.is_volume) {
            value = kernel::ctape_prim_dist(op, pr + CLAY_TAPE_PRIM_HEADER, blob, lp) + 0.0f;
        } else {
            value = kernel::ctape_prim_local(op, pr, blob, lp, CLAY_OUTARG(v.color));
        }
    } else if (h.repeat_radial) {
        const float d0 = kernel::ctape_prim_local(op, pr, blob,
                                                  kernel::ctape_repeat_point(h.repeat, lp, 0),
                                                  CLAY_OUTARG(repeat_color));
        const int neighbour =
            kernel::crep_radial_neighbor(lp, CLAY_INT(CLAY_AT(h.repeat, 1)));
        const float d1 = kernel::ctape_prim_local(
            op, pr, blob, kernel::ctape_repeat_point(h.repeat, lp, neighbour),
            CLAY_OUTARG(repeat_color));
        value = kernel::cmin(d0, d1);
    } else {
        value = kernel::ctape_prim_local(op, pr, blob,
                                         kernel::ctape_repeat_point(h.repeat, lp, 0),
                                         CLAY_OUTARG(repeat_color));
    }
    v.d = value * h.scale - h.round;
    return v;
}

// A combine instruction's parameter header, decoded once per block.
struct CombineHeader {
    int mode;
    int profile;
    float k;
    float rb;
    int gate_off;
    bool transition;
    const float* extra;  // mode-specific payload, past the shared prefix
};

CombineHeader decode_combine(const float* pr) {
    CombineHeader c;
    c.mode = CLAY_INT(pr[0]);
    c.profile = CLAY_INT(pr[1]);
    c.k = pr[2];
    c.rb = pr[3];
    c.gate_off = CLAY_INT(pr[4]);
    c.transition = kernel::ctape_mode_is_transition(c.mode);
    c.extra = pr + CLAY_TAPE_COMBINE_HEADER;
    return c;
}

// The combine branch's per-point half, transcribed from `ctape_eval`.
CTapeValue combine_at(const CombineHeader& c, const float* blob, CTapeValue a, CTapeValue b,
                      cfloat3 p) {
    CTapeValue combined;
    if (c.transition) {
        const float w = kernel::ctape_transition_weight(c.mode, c.extra, p);
        combined.d = kernel::cmix(a.d, b.d, w);
        combined.color = kernel::cmix(a.color, b.color, w);
    } else if (c.mode == kernel::ccombine_replace_feather) {
        combined = kernel::ctape_replace_feather(a, b, c.extra, blob, p);
    } else {
        combined = kernel::ctape_combine_values(a, b, c.mode, c.profile, c.k, c.rb);
    }
    if (c.gate_off >= 0) {
        const float g = kernel::ctape_gate_weight(blob + c.gate_off, blob, p);
        // The fully-protected end is a branch rather than an arithmetic
        // accident, for the reason the scalar path states: cmix(x, y, 1) is
        // y only up to rounding, and that last ULP is a seam along the whole
        // border of every protected region.
        if (g >= 1.0f) {
            combined = a;
        } else {
            combined.d = kernel::cmix(combined.d, a.d, g);
            combined.color = kernel::cmix(combined.color, a.color, g);
        }
    }
    return combined;
}

}  // namespace

std::size_t tape_stack_depth(const scene::Tape& tape) {
    std::size_t top = 0;
    std::size_t depth = 0;
    for (const kernel::CTapeInstr& instr : tape.instrs) {
        if (instr.op == kernel::ctape_combine) {
            if (top < 1) continue;
            if (top >= 2) --top;
        } else {
            if (top >= static_cast<std::size_t>(CLAY_TAPE_MAX_STACK)) continue;
            ++top;
        }
        depth = std::max(depth, top);
    }
    return depth;
}

void eval_points_blocked(const scene::Tape& tape, const PointQuery& q, const PointResults& out,
                         std::size_t block) {
    if (block == 0) block = kDefaultBlock;
    const std::size_t ni = tape.instrs.size();
    const std::size_t depth = tape_stack_depth(tape);

    // An empty tape — or one whose instructions never push — evaluates to "far
    // outside" for every point, which is what the scalar path's `top == 0`
    // return says. Handled here so the walk below can assume a stack.
    if (ni == 0 || depth == 0) {
        for (std::size_t i = 0; i < q.count; ++i) {
            if (out.distances) out.distances[i] = CLAY_TAPE_FAR;
            if (out.colors_rgb) {
                out.colors_rgb[i * 3] = 0.5f;
                out.colors_rgb[i * 3 + 1] = 0.5f;
                out.colors_rgb[i * 3 + 2] = 0.5f;
            }
        }
        return;
    }

    const kernel::CTapeInstr* in = tape.instrs.data();
    const float* params = tape.params.data();
    const float* blob = tape.blob.data();

    // Sized to the work actually present, not to `block`: the grid paths
    // dispatch chunks as small as one lattice ROW — 8 points for a brick — and
    // allocating a full block for those was 16 KB of malloc and zero-fill per 8
    // points of evaluation. That alone made the blocked path SLOWER than the
    // scalar one end to end, by more than the walk saved.
    //
    // Thread-local so the allocation happens once per thread rather than once
    // per call, and `resize` keeps the capacity: every slot is written by the
    // instruction that pushes it before anything reads it, so the growth path's
    // zero-fill is incidental rather than required.
    const std::size_t span = std::min(block, q.count);
    static thread_local std::vector<CTapeValue> stack;
    if (stack.size() < span * depth) stack.resize(span * depth);
    block = span;

    for (std::size_t base = 0; base < q.count; base += block) {
        const std::size_t n = std::min(block, q.count - base);
        std::size_t top = 0;
        for (std::size_t k = 0; k < ni; ++k) {
            const kernel::CTapeInstr& instr = in[k];
            const float* pr = params + instr.param_offset;
            if (instr.op == kernel::ctape_combine) {
                if (top < 1) continue;
                const CombineHeader c = decode_combine(pr);
                CTapeValue* b = &stack[(top - 1) * block];
                if (top >= 2) {
                    --top;
                    CTapeValue* a = &stack[(top - 1) * block];
                    // As above: the plain combine — no spatial transition, no
                    // feathered replace, no gate — gets its own loop chosen once
                    // per block rather than three tests per point.
                    if (!c.transition && c.mode != kernel::ccombine_replace_feather &&
                        c.gate_off < 0) {
                        for (std::size_t j = 0; j < n; ++j)
                            a[j] = kernel::ctape_combine_values(a[j], b[j], c.mode, c.profile,
                                                                c.k, c.rb);
                    } else {
                        for (std::size_t j = 0; j < n; ++j) {
                            const std::size_t i = base + j;
                            a[j] = combine_at(c, blob, a[j], b[j],
                                              cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1],
                                                  q.points_xyz[i * 3 + 2]));
                        }
                    }
                } else {
                    // A combine with an empty accumulator applies against empty
                    // space, seeding a chain whose earlier items were culled.
                    for (std::size_t j = 0; j < n; ++j) {
                        const std::size_t i = base + j;
                        CTapeValue a;
                        a.d = CLAY_TAPE_FAR;
                        a.color = b[j].color;
                        b[j] = combine_at(c, blob, a, b[j],
                                          cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1],
                                              q.points_xyz[i * 3 + 2]));
                    }
                }
            } else {
                if (top >= static_cast<std::size_t>(CLAY_TAPE_MAX_STACK)) continue;
                const PrimHeader h = decode_prim(pr, instr.op);  // once per block
                CTapeValue* s = &stack[top * block];
                // The branch is taken ONCE PER BLOCK, not once per point. That
                // is what hoisting a per-instruction property means: deciding
                // the value early still leaves a test and a generic call in the
                // inner loop, and measured, that was most of the win. The
                // specialised loop below is the common instruction — no repeat,
                // no deformer chain, not a sampled volume — written out so the
                // compiler sees a loop with no branches in it.
                if (!h.repeat_active && !h.has_deformers && !h.is_volume) {
                    for (std::size_t j = 0; j < n; ++j) {
                        const std::size_t i = base + j;
                        const cfloat3 lp = kernel::cmul_point(
                            h.inv, cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1],
                                       q.points_xyz[i * 3 + 2]));
                        s[j].color = h.color;
                        // `+ 0.0f` is the deformer offset the general path adds.
                        // Kept, not folded: dropping it turns a -0.0f into +0.0f,
                        // a different bit pattern, and the identity test says so.
                        s[j].d = (kernel::ctape_prim_dist(instr.op, pr + CLAY_TAPE_PRIM_HEADER,
                                                          blob, lp) +
                                  0.0f) *
                                     h.scale -
                                 h.round;
                    }
                } else {
                    for (std::size_t j = 0; j < n; ++j) {
                        const std::size_t i = base + j;
                        s[j] = prim_at(instr.op, pr, blob, h,
                                       cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1],
                                           q.points_xyz[i * 3 + 2]));
                    }
                }
                ++top;
            }
        }
        const CTapeValue* result = &stack[(top - 1) * block];
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = base + j;
            if (out.distances) out.distances[i] = result[j].d;
            if (out.colors_rgb) {
                out.colors_rgb[i * 3] = result[j].color.x;
                out.colors_rgb[i * 3 + 1] = result[j].color.y;
                out.colors_rgb[i * 3 + 2] = result[j].color.z;
            }
        }
    }
}

}  // namespace eval
}  // namespace clay
