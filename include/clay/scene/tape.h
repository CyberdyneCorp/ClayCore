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
    // grew from, not its bytes, and the contract here is about bytes. So an
    // appended dab is still a miss in a backend's resident-tape cache and
    // still costs a full re-upload — reusing the prefix makes the CPU
    // re-emission cheap and does nothing for the GPU. Fixing that needs this
    // identity to carry a generation and a changed range instead of a fresh
    // id, which is the phase this one exists to make expressible.
    std::uint64_t compile_id = 0;

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
// The chain pad is the largest single-item reach in the layer. That closed
// every case measured, at chain lengths from 5 to 600, but it is not a proof:
// the drag grows with chain length and no fixed dilation bounds it for an
// arbitrary document. Hard unions have no such term — min() is exact and
// associative — and measure identical at any length.
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
struct TapeCheckpoint {
    // Prefix lengths, not the tape's own sizes: any trailing layer union sits
    // after these and is re-emitted rather than reused.
    std::size_t instrs = 0;
    std::size_t params = 0;
    std::size_t blob = 0;
    LayerId layer = 0;      // the layer whose chain the prefix ends in
    bool layer_have_acc = false;  // that chain left a value on the stack
    bool doc_have_acc = false;    // an EARLIER layer left one underneath it
    bool valid = false;           // false when no layer was compiled at all
};

// The whole-document compile, plus the checkpoint an append can resume from.
// The tape is byte-identical to compile_document(doc) — this only records
// where the compile passed through.
Tape compile_document_resumable(const Document& doc, TapeCheckpoint* out_checkpoint);

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
bool compile_document_append(const Tape& prefix, const TapeCheckpoint& checkpoint,
                             const Document& doc, const std::vector<NodeId>& appended,
                             Tape* out, TapeCheckpoint* out_checkpoint);

}  // namespace scene
}  // namespace clay
