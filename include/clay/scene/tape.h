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

}  // namespace scene
}  // namespace clay
