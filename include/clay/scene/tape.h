#pragma once

// Tape compilation: ordered edit lists -> the flat postfix tape of
// kernel/tape.h. Transforms are pre-inverted, parameter edits rewrite only
// param blocks, and per-brick culling drops items whose influence bound
// misses the (band-dilated) region — the Dreams design (scene-model spec).

#include <cstdint>
#include <vector>

#include "clay/kernel/exactness.h"
#include "clay/kernel/tape.h"
#include "clay/math/geom.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

struct Tape {
    std::vector<kernel::CTapeInstr> instrs;
    std::vector<float> params;
    std::vector<float> blob;  // out-of-line payload: stroke points, polygon verts
    kernel::CFieldInfo info{true, 1.0f};
    math::Aabb bounds;  // union of item influence bounds (raycast clipping)

    // Content identity for backend upload caching. compile_document and
    // compile_layer stamp each tape they return with a process-unique nonzero
    // id, so two tapes with the same nonzero id carry byte-identical
    // instrs/params/blob (copies of a tape share the id WITH the bytes). A
    // backend may therefore keep the uploaded form resident and key it on
    // this id alone — no hashing, no false hits. 0 means "no identity":
    // hand-assembled tapes are never cached. Anything that mutates a compiled
    // tape's instrs/params/blob must reset this to 0; nothing in the library
    // mutates a compiled tape today.
    //
    // A tape built by REUSING another's prefix (compile_document_append) is no
    // exception and gets its own fresh id: it shares a prefix with the tape it
    // grew from, not its bytes, and the contract here is about bytes. What it
    // shares is recorded separately, in the lineage fields below, which is how
    // a backend serves an appended dab without re-uploading the tape.
    std::uint64_t compile_id = 0;

    // -- lineage: what this tape shares with the one it grew from ------------
    //
    // A tape built by reusing another's prefix (compile_document_append)
    // names that tape here, together with the offset in each section at which
    // the two stop agreeing. BELOW those offsets the two tapes are
    // byte-identical; at and above them this tape is its own.
    //
    // That is what lets a backend holding the ancestor resident transfer only
    // what changed instead of the whole tape — an appended dab adds ~148
    // bytes to a tape that is 7.8 MiB at 50,000 items, and without this
    // nothing downstream can tell that all but 148 of those bytes are the
    // ones it already uploaded.
    //
    // 0 means NO LINEAGE, the way compile_id 0 means no identity: every
    // compile entry point but the appending one leaves it there, and a
    // backend must then treat the tape as entirely new.
    //
    // THIS IS A CLAIM ABOUT BYTES, AND A FALSE ONE IS SILENT. A backend that
    // patches on a lineage that does not hold evaluates a field that never
    // existed — no error, no crash, just wrong answers. So it is set in
    // exactly one place, from the checkpoint that was actually used to build
    // the tape, which makes it true by construction rather than by
    // inspection. Anything that mutates a compiled tape's sections must clear
    // it along with compile_id.
    std::uint64_t parent_id = 0;
    std::size_t agree_instrs = 0;
    std::size_t agree_params = 0;
    std::size_t agree_blob = 0;

    float safe_step_scale() const { return kernel::csafe_step_scale(info); }
    bool empty() const { return instrs.empty(); }

    kernel::CTapeValue eval(kernel::cfloat3 p) const {
        return kernel::ctape_eval(instrs.data(), static_cast<int>(instrs.size()), params.data(),
                                  blob.data(), p);
    }
};

// Optional culling region for per-brick tapes. The caller dilates the brick
// AABB by its narrow-band width; only items whose influence bound touches
// the dilated region are compiled. Band-clamped results are bit-identical
// to the full tape inside the region.
//
// The compiler adds its own pad on top of the caller's region (scene::cull_pad)
// and the caller should NOT try to. Two things reach further than an item's own
// bound:
//
//   - a feathered replace, whose crossfade steers a value from up to its
//     volume's band away, and
//   - a SMOOTH-UNION CHAIN, which is the subtler one. An item's bound is
//     dilated by what one blend can move; but the accumulated value part way
//     down a long chain sits well above where it ends up, so an item whose
//     final contribution is nothing can still be within k of the RUNNING value
//     and change it. Measured on a 600-dab sphere at k=0.06 the chain drags
//     the field more than four k below the base shape's own distance, and
//     without the pad, samples INSIDE the band differed from the full tape by
//     up to half a cell.
//
// The chain pad is min(support, k * envelope(N)) per item, maximized over the
// layer, with N the layer's EFFECTIVE contributor count — node count times
// the symmetry multiplicity, since a mirrored item is compiled once per
// mirror/radial copy and every copy is a real contributor to the one serial
// chain — and the envelope a measured per-profile fit that RISES with N
// (bounds.cpp records both campaigns) — because the sufficient pad does: no
// fixed k-multiple closes every length, and the support clamp is what keeps
// the pad at or under the pre-#335 `max(support, k)` everywhere. The seam
// blends the copies enter through fold in as a quadratic term of the LAYER's
// seam k, clamped at the ceiling the item blends alone resolve to — the
// pre-#335 pad — so a seam wider than any item k pads the chain without ever
// exceeding that pad. The fit clears every measured knee, to 8000 nodes and
// to 64-fold radial amplification, with the margin the knees drift by across
// seed draws; it is not a proof — the drag grows with chain length and no
// fixed dilation bounds it for an arbitrary document, which is why the
// clamp, not the fit, is the last word. Hard unions have no such term —
// min() is exact and associative — and measure identical at any length.
struct CullRegion {
    math::Aabb region;
};

class CullIndex;
class CullPlan;

// Whole document: visible SDF layers chained by hard union.
//
// `index` (cull_index.h) supplies per-revision cached bounds; `plan` a
// per-batch coarse cull, valid only with a `cull` region contained in the
// plan's own. Both accelerate the compile without changing its output: the
// tape is byte-identical with and without them.
Tape compile_document(const Document& doc, const CullRegion* cull = nullptr,
                      const CullIndex* index = nullptr, const CullPlan* plan = nullptr);

// Single layer.
Tape compile_layer(const Layer& layer, const CullRegion* cull = nullptr);

// -- resuming a whole-document compile after an append -----------------------
//
// A sculpt grows by one node per brush stamp, and a host that raycasts to
// place the next dab recompiles the whole document to do it: 99,999
// instructions and 7.8 MiB re-emitted at 50,000 items so that ~148 bytes can
// go on the end. Everything already emitted is still correct — the compiler
// emits items left to right and nothing it has written moves — so the prefix
// can be reused and only the appended nodes compiled.
//
// The reuse is sound because of what does NOT happen: an item's
// `param_offset` and its blob handles are absolute offsets into the final
// buffers, and appending never moves what is in front of it, so a reused
// prefix needs no relocation pass at all.
//
// WHERE A COMPILE CAN BE RESUMED FROM is not the end of the tape. `run()`
// compiles each visible SDF layer's chain against its own fresh accumulator
// and then folds it into the layers below with a hard union emitted AFTER
// that chain, so with more than one visible layer the tape ends in a union
// that an appended item has to be emitted BEFORE. The checkpoint is therefore
// a truncation point: the tape lengths at the end of the last visible SDF
// layer's chain, plus the two accumulator flags needed to carry on from
// there. Resuming copies the tape up to those lengths, compiles the appended
// nodes onto it, and re-emits the union.
// One GROUP the prefix ends inside, and what finishing its chain costs.
//
// A checkpoint used to sit only at the end of a layer's root list, where the
// one thing left to re-emit was the union with the layers below. A checkpoint
// inside a group sits in front of a STACK: that group's combine, then each
// enclosing group's, then the union. compile_group emits its combine AFTER
// its children, so this is what the prefix has not paid for yet.
struct TapeCheckpointFrame {
    NodeId group = kNoNode;
    // The chain CONTAINING this group, as it stood when the group was entered.
    // compile_group decides whether to emit on `have_acc || seeded`, and both
    // halves of that are here rather than re-derived, because re-deriving them
    // means replaying the chain this checkpoint exists to avoid replaying.
    bool outer_have_acc = false;
    bool seeded = false;
    // Whether finishing this group's chain emits a combine at all. FALSE for
    // an inner Add group entered with nothing beneath it: its children's value
    // IS the chain's value and no combine is emitted. Measured — nesting
    // all-Add groups four deep leaves the same single trailing instruction as
    // one, which is why "one combine per level" is wrong.
    bool emits = false;
    Op op = Op::Add;
    Blend blend{};
    float rounding = 0.0f;  // already scaled by the layer transform
};

// How many stack planes a walk holds when it reaches the checkpoint: the
// chain's own accumulator, plus one for each enclosing group that actually
// emits a combine. NOT `frames.size() + 1` -- a frame with `emits == false`
// opens no outer slot, which is the ordinary case for a group that is the
// first thing in its layer. Every producer and consumer of a stack SHALL size
// it with this, or the two disagree and the seed is silently dropped.
// `layer_have_acc` is the checkpoint's own: FALSE where the chain the
// checkpoint sits in has not produced a value yet, which is what a tail group
// that was EMPTY at compile time leaves behind. Then the bottom plane is the
// chain OUTSIDE the group and there is no plane for the chain itself.
inline std::size_t checkpoint_stack_levels(const std::vector<TapeCheckpointFrame>& frames,
                                           bool layer_have_acc = true) {
    std::size_t levels = layer_have_acc ? 1u : 0u;
    for (const TapeCheckpointFrame& f : frames)
        if (f.emits) ++levels;
    return levels;
}

struct TapeCheckpoint {
    // Prefix lengths, not the tape's own sizes: any trailing layer union sits
    // after these and is re-emitted rather than reused.
    std::size_t instrs = 0;
    std::size_t params = 0;
    std::size_t blob = 0;
    LayerId layer = 0;      // the layer whose chain the prefix ends in
    bool layer_have_acc = false;  // the INNERMOST chain left a value on the stack
    bool doc_have_acc = false;    // an EARLIER layer left one underneath it
    bool valid = false;           // false when no layer was compiled at all
    // The groups the prefix ends inside, INNERMOST FIRST, excluding the root
    // list itself. Empty when the checkpoint is at the layer's root list,
    // which is what every checkpoint was before group appends existed — so an
    // empty stack is exactly the old behaviour and not a special case.
    std::vector<TapeCheckpointFrame> frames;
};

// The whole-document compile, plus the checkpoint an append can resume from.
// The tape is byte-identical to compile_document(doc) — this only records
// where the compile passed through.
Tape compile_document_resumable(const Document& doc, TapeCheckpoint* out_checkpoint);

// The same, CULLED -- byte-identical to compile_document(doc, cull, index,
// plan), plus the checkpoint that compile passed through.
//
// A brick refill needs both halves at once. What it stores as a seed is the
// stack where the checkpoint sits, not the value the walk ends with: inside a
// group the group's combine is still pending, and the finished field has it
// folded in already. The uncalled variant above cannot serve that, because a
// seed taken without the brick's cull is continued from a different field
// than the one the brick evaluates.
Tape compile_document_resumable(const Document& doc, TapeCheckpoint* out_checkpoint,
                                const CullRegion* cull, const CullIndex* index,
                                const CullPlan* plan);

// Compile `doc` by reusing `prefix`, which must have come from
// compile_document_resumable together with `checkpoint`, where `doc` differs
// from the document that produced them ONLY by `appended` at the tail of
// checkpoint.layer's root list. Writes the tape to `out` and returns true.
//
// Returns false, leaving `out` untouched, when the checkpoint does not apply
// — no valid checkpoint, the layer is gone or is no longer the last visible
// SDF layer, or the prefix is shorter than the checkpoint claims. The caller
// then compiles in full. Refusing costs a recompile, which is what it would
// have paid anyway; reusing a prefix that has moved is silent and wrong, so
// this refuses wherever it is not certain.
//
// The result is bit-identical to compile_document(doc) — instrs, params,
// blob, info and bounds — and carries its own fresh identity, because its
// bytes differ from the prefix it reused. `prefix` is not modified.
// `out_checkpoint` receives the checkpoint for the tape just built, so the
// next append resumes from this one rather than falling back to a full
// compile — which is what makes a stroke, rather than one dab, cheap.
//
// The tape it writes also carries its LINEAGE (Tape::parent_id and the three
// agree_* offsets): `prefix`'s identity, and the checkpoint's lengths, which
// are exactly the point up to which the two tapes agree.
bool compile_document_append(const Tape& prefix, const TapeCheckpoint& checkpoint,
                             const Document& doc, const std::vector<NodeId>& appended,
                             Tape* out, TapeCheckpoint* out_checkpoint);

// -- compiling only the SUFFIX, to be evaluated onto a value ------------------
//
// The same compile as `compile_document_append`, without the prefix. Where that
// one copies the prefix's bytes so the result stands alone, this emits ONLY the
// instructions for `appended` — a tape that expects the accumulator the prefix
// would have left, already on the stack, and folds onto it.
//
// What that is for: a dab's cost currently follows everything the artist has
// already sculpted, because a dirty brick re-evaluates every surviving item
// over its samples even though almost none of them changed. Measured at a 0.05
// voxel with dabs spread evenly over a sphere, one dab into 12 bricks costs
// 0.23 ms at 200 items and 18.07 ms at 50,000 (#306). If the value the prefix
// produced at those lattice points is known, this is what the rest costs.
//
// SOUND BECAUSE THE CHAIN IS A FOLD AT ITEM BOUNDARIES. The compiler emits each
// item's contribution as a self-contained expression and then folds it into one
// running accumulator, so after every item the stack holds exactly one value —
// including with a layer mirror, where an item emits two prims and a combine
// before folding in. `TapeCheckpoint` already names the places that is true,
// which is why the validity question here is the one `compile_document_append`
// answers rather than a new one.
//
// Continuing from that value is not an approximation of evaluating the whole
// chain, it IS evaluating the whole chain: the same instructions in the same
// order over the same floats, with the ones already folded represented by the
// number they produced. Where the seed is exact the result is bit-identical,
// and `test_suffix_tape.cpp` holds that rather than a tolerance.
//
// Refuses on the same terms and for the same reason: no valid checkpoint, the
// layer gone or no longer the last visible SDF layer, or `appended` not
// actually the tail of its roots. A caller that is refused evaluates in full.
//
// THE TAPE IS NOT SELF-CONTAINED and must not be handed to a plain evaluator.
// Its `bounds` and `info` describe the appended items only, and evaluating it
// with an empty stack yields the suffix against empty space rather than against
// the shape. `eval::eval_points_seeded` is what reads it.
//
// `cull` (with `index` for its pad, as compile_document takes them) drops
// appended items a region cannot reach, exactly as a whole-document compile
// would -- which is REQUIRED rather than an optimisation when the value being
// folded onto was itself computed under that cull. A suffix culled differently
// from the prefix it continues is a different field, and only outside the band,
// which is where nothing is looking.
// -- one half of a document, for a resumable multi-layer refill --------------
//
// A document's visible SDF layers hard-union left to right, so a compile that
// stops before `active` and one that emits only `active` are, together, the
// whole document apart from that union. A caller that holds the two VALUES can
// fold appended items into the second and union them itself, which is what a
// brick refill does when more than one layer is visible: the layers below are
// static across a stroke, and only the active one moves.
//
// `below` true emits the visible SDF layers BEFORE `active`; false emits only
// `active`. One half per call, because a caller that wants both wants them into
// two batches and one that wants one should not pay for the other.
//
// Both halves cull under the WHOLE DOCUMENT's pad, not their own. A part
// compiled under a smaller pad drops items the whole-document compile keeps,
// and then the two halves no longer sum to the whole.
//
// The union to fold them with is a HARD Add -- `ctape_combine_values` with the
// Add mode and no blend -- which is what the whole-document compile emits
// between layers. Anything else is a different field.
Tape compile_document_part(const Document& doc, LayerId active, bool below,
                           const CullRegion* cull = nullptr, const CullIndex* index = nullptr);

// The same, plus the checkpoint it passed through -- what a brick refill needs
// to store the stack rather than the answer. For the ACTIVE half that
// checkpoint is the one a suffix resumes from, which is why a part has to be
// able to report it at all.
Tape compile_document_part_resumable(const Document& doc, LayerId active, bool below,
                                     const CullRegion* cull, const CullIndex* index,
                                     TapeCheckpoint* out_checkpoint);

// The OTHER pairing: every visible SDF layer EXCEPT `excluded`, wherever it
// sits in the stack. `compile_document_part`'s `below` STOPS at the named layer
// and so drops everything above it too; this one skips it and keeps walking.
// With `compile_document_part(doc, excluded, /*below=*/false)` it sums to the
// whole document under the same hard union — which is what a host previewing
// one layer needs in order to draw the rest of the document beside it (#378).
//
// Culls under the WHOLE DOCUMENT's pad, exactly as the other parts do and for
// the same reason: a part compiled under its own smaller pad drops items the
// whole-document compile keeps, and the parts then no longer sum to the whole.
//
// A layer that is hidden, is not an SDF layer, or is not in the document is
// already contributing nothing, so excluding it is an ordinary compile of every
// visible SDF layer rather than an error. Whether a CALLER meant that is the C
// ABI's question, where the caller's intent is known.
//
// No checkpoint and no resumable form: a checkpoint names the layer a suffix
// continues, and the one layer this compile does not hold is the one it was
// given.
Tape compile_document_except(const Document& doc, LayerId excluded,
                             const CullRegion* cull = nullptr, const CullIndex* index = nullptr);

bool compile_layer_suffix(const TapeCheckpoint& checkpoint, const Document& doc,
                          const std::vector<NodeId>& appended, Tape* out,
                          TapeCheckpoint* out_checkpoint, const CullRegion* cull = nullptr,
                          const CullIndex* index = nullptr);

// The first `count` roots of the last visible SDF layer, compiled as a
// STANDALONE tape under the DOCUMENT's cull pad — the pad compile_layer_suffix
// computes (index->cull_pad(), else the max over visible SDF layers), NOT
// compile_layer's per-layer pad. The value this tape produces is the seed a
// suffix compiled by compile_layer_suffix will be folded onto, and prefix and
// suffix must cull under one pad or they describe two different fields.
//
// Evaluable by a plain evaluator — unlike a suffix tape it starts from
// nothing, so there is no accumulator for it to expect. Refuses on count == 0
// (an empty prefix is not a tape, it is the absence of one), count past the
// root list, or no visible SDF layer.
bool compile_layer_prefix(const Document& doc, std::size_t count, Tape* out,
                          const CullRegion* cull = nullptr,
                          const CullIndex* index = nullptr);

}  // namespace scene
}  // namespace clay
