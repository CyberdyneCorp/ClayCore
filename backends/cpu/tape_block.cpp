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
#include "clay/kernel/field.h"
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

namespace {

// What a stack slot holds. A distance-only query keeps ONE float per slot where
// a `CTapeValue` is four, and in a blocked walk the stack is an array across the
// block, so those bytes are the working set rather than a register.
template <bool WithColour>
struct SlotOf {
    using type = CTapeValue;
};
template <>
struct SlotOf<false> {
    using type = float;
};

// The walk, once, over whichever slot the caller's request selects.
//
// The specialisation is `if constexpr` INSIDE the loops rather than a policy
// object wrapping the values, and that is deliberate rather than stylistic. An
// earlier attempt routed every store through a policy's `store()` and cost 1.30x
// on the coloured path for no change in what it computed (#219). The rule this
// evaluator keeps teaching: specialise the loop, never the value.
template <bool WithColour>
// `levels` is how many stack slots the walk STARTS holding, and `stack_out`
// asks for the whole final stack instead of only its top.
//
// Both exist for a resume that picks up INSIDE a group. compile_group emits a
// group's combine after its children, so a prefix that stops there leaves one
// value per open group plus one on the stack, and the suffix's own combines
// pop them in order — the stack machine already does the folding, it just has
// to be handed more than one value to fold. One seed can only continue a
// chain that had nothing open above it.
void walk_blocked(const kernel::CTapeInstr* in, std::size_t ni, const float* params,
                  const float* blob, const PointQuery& q, const PointResults& out,
                  std::size_t block, std::size_t depth, const float* seed = nullptr,
                  const float* seed_rgb = nullptr, std::size_t levels = 1,
                  float* stack_out = nullptr, float* stack_out_rgb = nullptr,
                  std::size_t* stack_out_levels = nullptr) {
    using Slot = typename SlotOf<WithColour>::type;
    // Thread-local so the allocation happens once per thread rather than once
    // per call, and sized to the work present rather than to `block`: the grid
    // paths dispatch chunks as small as one lattice ROW — 8 points for a brick —
    // and allocating a full block for those was 16 KB of malloc and zero-fill
    // per 8 points of evaluation, which made the blocked path slower end to end
    // than the scalar one it replaced.
    static thread_local std::vector<Slot> stack;
    if (stack.size() < block * depth) stack.resize(block * depth);

    for (std::size_t base = 0; base < q.count; base += block) {
        const std::size_t n = std::min(block, q.count - base);
        const auto point = [&](std::size_t j) {
            const std::size_t i = base + j;
            return cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1], q.points_xyz[i * 3 + 2]);
        };
        std::size_t top = 0;
        // A SEEDED walk starts holding the value the instructions ahead of
        // these produced, so the first combine folds onto it exactly as it
        // would have.
        //
        // What the accumulator IS decides what a seed has to carry: a
        // distance-only walk folds one float a point, and a coloured one folds
        // a CTapeValue, so the colour has to come with it or the first combine
        // reads whatever was in the slot. `seed_rgb` is that colour, three
        // floats a point in the same order.
        if (seed) {
            // Plane `l` of the seed is stack slot `l`, so plane 0 is the
            // BOTTOM. That is the order compile_group left them in — the
            // outermost open chain first — and the order the suffix's combines
            // pop them in.
            for (std::size_t l = 0; l < levels; ++l) {
                Slot* sl = &stack[l * block];
                const float* plane = seed + l * q.count;
                const float* plane_rgb = seed_rgb ? seed_rgb + l * q.count * 3 : nullptr;
                for (std::size_t j = 0; j < n; ++j) {
                    const std::size_t i = base + j;
                    if constexpr (WithColour) {
                        sl[j].d = plane[i];
                        sl[j].color = plane_rgb ? cf3(plane_rgb[i * 3], plane_rgb[i * 3 + 1],
                                                      plane_rgb[i * 3 + 2])
                                                : cf3(0.0f, 0.0f, 0.0f);
                    } else {
                        sl[j] = plane[i];
                    }
                }
            }
            top = levels;
        }
        for (std::size_t k = 0; k < ni; ++k) {
            const kernel::CTapeInstr& instr = in[k];
            const float* pr = params + instr.param_offset;
            if (instr.op == kernel::ctape_combine) {
                if (top < 1) continue;
                const CombineHeader c = decode_combine(pr);
                Slot* b = &stack[(top - 1) * block];
                const bool plain = !c.transition &&
                                   c.mode != kernel::ccombine_replace_feather && c.gate_off < 0;
                if (top >= 2) {
                    --top;
                    Slot* a = &stack[(top - 1) * block];
                    if (plain) {
                        // The common combine, chosen once per block rather than
                        // three tests per point.
                        for (std::size_t j = 0; j < n; ++j) {
                            if constexpr (WithColour)
                                a[j] = kernel::ctape_combine_values(a[j], b[j], c.mode, c.profile,
                                                                    c.k, c.rb);
                            else
                                a[j] = kernel::ctape_combine_dist(a[j], b[j], c.mode, c.profile,
                                                                  c.k, c.rb);
                        }
                    } else {
                        // Gated, transitional or feathered: rare, and it goes
                        // through the CTapeValue form even for a distance-only
                        // query. Correct because no distance expression reads a
                        // colour, and affordable because these are the
                        // instructions a document has few of.
                        for (std::size_t j = 0; j < n; ++j) {
                            CTapeValue av;
                            CTapeValue bv;
                            if constexpr (WithColour) {
                                av = a[j];
                                bv = b[j];
                            } else {
                                av.d = a[j];
                                av.color = cf3(0.0f, 0.0f, 0.0f);
                                bv.d = b[j];
                                bv.color = cf3(0.0f, 0.0f, 0.0f);
                            }
                            const CTapeValue rv = combine_at(c, blob, av, bv, point(j));
                            if constexpr (WithColour)
                                a[j] = rv;
                            else
                                a[j] = rv.d;
                        }
                    }
                } else {
                    // A combine with an empty accumulator applies against empty
                    // space, seeding a chain whose earlier items were culled.
                    for (std::size_t j = 0; j < n; ++j) {
                        if constexpr (!WithColour) {
                            if (plain) {
                                b[j] = kernel::ctape_combine_dist(CLAY_TAPE_FAR, b[j], c.mode,
                                                                  c.profile, c.k, c.rb);
                                continue;
                            }
                        }
                        CTapeValue bv;
                        if constexpr (WithColour) {
                            bv = b[j];
                        } else {
                            bv.d = b[j];
                            bv.color = cf3(0.0f, 0.0f, 0.0f);
                        }
                        CTapeValue a;
                        a.d = CLAY_TAPE_FAR;
                        a.color = bv.color;
                        const CTapeValue rv = combine_at(c, blob, a, bv, point(j));
                        if constexpr (WithColour)
                            b[j] = rv;
                        else
                            b[j] = rv.d;
                    }
                }
            } else {
                if (top >= static_cast<std::size_t>(CLAY_TAPE_MAX_STACK)) continue;
                const PrimHeader h = decode_prim(pr, instr.op);  // once per block
                Slot* s = &stack[top * block];
                if (!h.repeat_active && !h.has_deformers && !h.is_volume) {
                    for (std::size_t j = 0; j < n; ++j) {
                        const cfloat3 lp = kernel::cmul_point(h.inv, point(j));
                        // `+ 0.0f` is the deformer offset the general path adds.
                        // Kept, not folded: dropping it turns a -0.0f into +0.0f,
                        // a different bit pattern, and the identity test says so.
                        const float d = (kernel::ctape_prim_dist(
                                             instr.op, pr + CLAY_TAPE_PRIM_HEADER, blob, lp) +
                                         0.0f) *
                                            h.scale -
                                        h.round;
                        if constexpr (WithColour) {
                            s[j].color = h.color;
                            s[j].d = d;
                        } else {
                            s[j] = d;
                        }
                    }
                } else {
                    for (std::size_t j = 0; j < n; ++j) {
                        const CTapeValue v = prim_at(instr.op, pr, blob, h, point(j));
                        if constexpr (WithColour)
                            s[j] = v;
                        else
                            s[j] = v.d;
                    }
                }
                ++top;
            }
        }
        // THE WHOLE STACK, when a caller asked for it: this is the prefix half
        // of a group resume, whose value is not one number but the open chains
        // the checkpoint stopped inside.
        if (stack_out) {
            if (stack_out_levels) *stack_out_levels = top;
            for (std::size_t l = 0; l < top; ++l) {
                const Slot* sl = &stack[l * block];
                for (std::size_t j = 0; j < n; ++j) {
                    const std::size_t i = base + j;
                    if constexpr (WithColour) {
                        stack_out[l * q.count + i] = sl[j].d;
                        if (stack_out_rgb) {
                            stack_out_rgb[(l * q.count + i) * 3] = sl[j].color.x;
                            stack_out_rgb[(l * q.count + i) * 3 + 1] = sl[j].color.y;
                            stack_out_rgb[(l * q.count + i) * 3 + 2] = sl[j].color.z;
                        }
                    } else {
                        stack_out[l * q.count + i] = sl[j];
                    }
                }
            }
        }
        const Slot* result = &stack[(top - 1) * block];
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = base + j;
            if constexpr (WithColour) {
                if (out.distances) out.distances[i] = result[j].d;
                if (out.colors_rgb) {
                    out.colors_rgb[i * 3] = result[j].color.x;
                    out.colors_rgb[i * 3 + 1] = result[j].color.y;
                    out.colors_rgb[i * 3 + 2] = result[j].color.z;
                }
            } else {
                if (out.distances) out.distances[i] = result[j];
            }
        }
    }
}

}  // namespace

namespace {
// `tape_stack_depth` for a walk that starts already holding one value. Not that
// one plus one: a combine is SKIPPED at depth zero and executed at depth one, so
// the two simulations do not run the same instructions and the deepest point of
// one is not the deepest point of the other shifted up.
std::size_t seeded_stack_depth(const scene::Tape& tape) {
    std::size_t top = 1;
    std::size_t depth = 1;
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
}  // namespace

void eval_points_seeded(const scene::Tape& suffix, const PointQuery& q, const float* seed,
                        const float* seed_rgb, const PointResults& out, std::size_t block) {
    if (block == 0) block = kDefaultBlock;
    if (!seed || !out.distances) return;
    // Colour is carried when the caller wants it back AND supplied the colour
    // the prefix reached. Asking for it without giving it would fold every
    // combine against black, which is a wrong answer rather than a missing one.
    const bool with_colour = out.colors_rgb != nullptr && seed_rgb != nullptr;
    PointResults d;
    d.distances = out.distances;
    if (with_colour) d.colors_rgb = out.colors_rgb;

    const std::size_t ni = suffix.instrs.size();
    // An empty suffix is the prefix, unchanged -- which is what the seed
    // already is. Not "far outside": the stack is not empty here.
    if (ni == 0) {
        for (std::size_t i = 0; i < q.count; ++i) out.distances[i] = seed[i];
        if (with_colour)
            for (std::size_t i = 0; i < q.count * 3; ++i) out.colors_rgb[i] = seed_rgb[i];
        return;
    }
    const std::size_t depth = seeded_stack_depth(suffix);
    const std::size_t span = std::min(block, q.count);
    if (with_colour)
        walk_blocked<true>(suffix.instrs.data(), ni, suffix.params.data(), suffix.blob.data(), q, d,
                           span, depth, seed, seed_rgb);
    else
        walk_blocked<false>(suffix.instrs.data(), ni, suffix.params.data(), suffix.blob.data(), q,
                            d, span, depth, seed, nullptr);
}

void eval_points_seeded_stack(const scene::Tape& suffix, const PointQuery& q, const float* seeds,
                              const float* seeds_rgb, std::size_t levels,
                              const PointResults& out, std::size_t block) {
    if (block == 0) block = kDefaultBlock;
    if (!seeds || !out.distances || levels == 0) return;
    if (levels > static_cast<std::size_t>(CLAY_TAPE_MAX_STACK)) return;
    const bool with_colour = out.colors_rgb != nullptr && seeds_rgb != nullptr;
    PointResults d;
    d.distances = out.distances;
    if (with_colour) d.colors_rgb = out.colors_rgb;

    const std::size_t ni = suffix.instrs.size();
    if (ni == 0) {
        // An empty suffix leaves the stack as it was, so the answer is its TOP
        // plane -- not plane 0, which is the bottom and is what a one-level
        // caller's seed happens to be.
        const float* top_plane = seeds + (levels - 1) * q.count;
        for (std::size_t i = 0; i < q.count; ++i) out.distances[i] = top_plane[i];
        if (with_colour) {
            const float* top_rgb = seeds_rgb + (levels - 1) * q.count * 3;
            for (std::size_t i = 0; i < q.count * 3; ++i) out.colors_rgb[i] = top_rgb[i];
        }
        return;
    }
    // The walk starts `levels` deep rather than one, so the depth it can reach
    // is that much more than a one-seed walk of the same instructions.
    const std::size_t depth = seeded_stack_depth(suffix) + (levels - 1);
    const std::size_t span = std::min(block, q.count);
    if (with_colour)
        walk_blocked<true>(suffix.instrs.data(), ni, suffix.params.data(), suffix.blob.data(), q, d,
                           span, depth, seeds, seeds_rgb, levels);
    else
        walk_blocked<false>(suffix.instrs.data(), ni, suffix.params.data(), suffix.blob.data(), q,
                            d, span, depth, seeds, nullptr, levels);
}

void eval_points_stack(const scene::Tape& tape, const PointQuery& q, float* stack_out,
                       float* stack_out_rgb, std::size_t* out_levels, std::size_t block) {
    if (block == 0) block = kDefaultBlock;
    if (!stack_out) return;
    if (out_levels) *out_levels = 0;
    const std::size_t ni = tape.instrs.size();
    if (ni == 0) return;  // nothing on the stack: no levels, nothing written
    const std::size_t depth = tape_stack_depth(tape);
    const std::size_t span = std::min(block, q.count);
    // `out` is unused by a stack read-out but walk_blocked writes its top
    // through it, so it is given somewhere to land rather than a null it would
    // have to test per point.
    std::vector<float> ignored(q.count);
    PointResults d;
    d.distances = ignored.data();
    if (stack_out_rgb)
        walk_blocked<true>(tape.instrs.data(), ni, tape.params.data(), tape.blob.data(), q, d, span,
                           depth, nullptr, nullptr, 1, stack_out, stack_out_rgb, out_levels);
    else
        walk_blocked<false>(tape.instrs.data(), ni, tape.params.data(), tape.blob.data(), q, d,
                            span, depth, nullptr, nullptr, 1, stack_out, nullptr, out_levels);
}

void eval_points_blocked(const scene::Tape& tape, const PointQuery& q, const PointResults& out,
                         std::size_t block) {
    if (block == 0) block = kDefaultBlock;

    // GRADIENTS: four tetrahedron taps, each of which is the same block of
    // points displaced by a constant. So a gradient is four more BLOCKED walks
    // rather than four more per-point ones — the same count of evaluations the
    // scalar path does, through the path that walks the tape once per block.
    //
    // The taps, the weighted sum and the normalize are `kernel::cnormal`'s,
    // written out here because it takes a callable per point and this needs the
    // four taps as four arrays. Same expressions in the same order, so the
    // result is bit-identical — which is what lets `eval_points_reference` stay
    // the definition of correctness for the gradient path too.
    if (out.gradients_xyz) {
        // The base walk produces the DISTANCE and the COLOUR at each point. The
        // four taps below produce the gradient and need neither, so when a
        // caller asked for neither this walk is a fifth of the work for an
        // answer nobody reads -- which is exactly what a brick mesh asking for
        // gradient normals without colours was paying.
        if (out.distances || out.colors_rgb) {
            PointResults base = out;
            base.gradients_xyz = nullptr;
            eval_points_blocked(tape, q, base, block);
        }

        const cfloat3 e[4] = {cf3(1.0f, -1.0f, -1.0f), cf3(-1.0f, -1.0f, 1.0f),
                              cf3(-1.0f, 1.0f, -1.0f), cf3(1.0f, 1.0f, 1.0f)};
        static thread_local std::vector<float> taps;
        static thread_local std::vector<float> dist;
        for (std::size_t start = 0; start < q.count; start += block) {
            const std::size_t n = std::min(block, q.count - start);
            if (taps.size() < n * 3) taps.resize(n * 3);
            if (dist.size() < n * 4) dist.resize(n * 4);
            for (int k = 0; k < 4; ++k) {
                for (std::size_t j = 0; j < n; ++j) {
                    const std::size_t i = start + j;
                    const cfloat3 p = cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1],
                                          q.points_xyz[i * 3 + 2]);
                    const cfloat3 t = p + e[k] * q.gradient_eps;
                    taps[j * 3] = t.x;
                    taps[j * 3 + 1] = t.y;
                    taps[j * 3 + 2] = t.z;
                }
                PointQuery tq{taps.data(), n, q.gradient_eps};
                PointResults tr;
                tr.distances = dist.data() + static_cast<std::size_t>(k) * n;
                tr.gradients_xyz = nullptr;
                tr.colors_rgb = nullptr;
                eval_points_blocked(tape, tq, tr, block);
            }
            for (std::size_t j = 0; j < n; ++j) {
                const cfloat3 nn = e[0] * dist[j] + e[1] * dist[n + j] +
                                   e[2] * dist[2 * n + j] + e[3] * dist[3 * n + j];
                const cfloat3 g = kernel::cnormalize(nn);
                const std::size_t i = start + j;
                out.gradients_xyz[i * 3] = g.x;
                out.gradients_xyz[i * 3 + 1] = g.y;
                out.gradients_xyz[i * 3 + 2] = g.z;
            }
        }
        return;
    }

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
    if (out.colors_rgb)
        walk_blocked<true>(in, ni, params, blob, q, out, span, depth);
    else
        walk_blocked<false>(in, ni, params, blob, q, out, span, depth);
}

}  // namespace eval
}  // namespace clay
