#pragma once

// Tape compilation: ordered edit lists -> the flat postfix tape of
// kernel/tape.h. Transforms are pre-inverted, parameter edits rewrite only
// param blocks, and per-brick culling drops items whose influence bound
// misses the (band-dilated) region — the Dreams design (scene-model spec).

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
