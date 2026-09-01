#pragma once

// HIGH-DETAIL STAMPS: height maps and tangent-space vector displacement, over
// the alpha square the mesh brushes already project (mesh-sculpt-layers spec,
// add-mesh-sculpt-layers).
//
// WHAT IS NEW HERE IS ONE THING ONLY: what the sampled value MEANS.
//
//   Weight        a scalar in [0,1] multiplying the brush's per-vertex weight.
//                 This is `MeshBrushSettings::alpha`, unchanged, and it is here
//                 so the three modes can be spelled in one enum rather than
//                 living in two places.
//   Height        a signed displacement along the vertex's own normal, in
//                 world units, scaled by `amplitude`.
//   Vector        three components read IN THE VERTEX'S TRANSPORTED FRAME —
//                 tangent, bitangent, normal — so one stamp carries overhangs,
//                 undercuts and lateral flow that a height field cannot.
//
// THE PROJECTION AND THE SAMPLER ARE THE EXISTING ONES. `kernel::calpha_frame`
// places the square and `kernel::calpha_sample` reads it, exactly as
// `alpha_frame_for` and `alpha_at` do for the scalar alpha, and
// `MeshBrushSettings`' `alpha_direction`, `alpha_tangent` and `alpha_extent`
// place it with the same meanings. A second sampler with its own orientation
// rules was the obvious alternative and was rejected for the cost the existing
// comment already names: two bilinear lookups that drift apart, so that a stamp
// reads one way on a mesh and another way on a field.
//
// VECTOR DISPLACEMENT IS NEVER WORLD-SPACE, and that is not a preference. A
// world-space stamp is orientation-dependent: the same displacement map applied
// to the same feature on the left and right of a face produces two different
// shapes, and applied across a curved surface it shears. That is D2 of
// `add-mesh-multires` re-derived for stamps — the same argument that made
// detail itself frame-relative — and it is why this reads the SAME frame the
// coefficients are stored in, so a stamp and a stroke deposit into one
// representation.
//
// THE IMAGE IS PLANAR AND BORROWED. Three channels means three consecutive
// `width * height` planes, not interleaved triples, because a plane is exactly
// the buffer `calpha_sample` already reads — interleaving would need a second
// lookup with its own stride rules, which is the drift this file exists to
// avoid. The engine decodes no images and copies nothing: a host that has
// decoded a map points at it for the duration of the call.
//
// THIS FILE KNOWS NOTHING ABOUT LAYERS. It turns a placement and an image into
// three coefficients at a point, and the sculptor decides where they go — which
// is what makes it testable without a hierarchy.

#include <cstdint>

#include "clay/kernel/shim.h"
#include "clay/mesh/detail_field.h"
#include "clay/mesh/sculpt_kernels.h"
#include "clay/mesh/surface_frame.h"

namespace clay {
namespace mesh {

enum class DetailStampMode : std::uint32_t {
    Weight = 0,
    Height = 1,
    Vector = 2,
};

struct DetailStampSettings {
    DetailStampMode mode = DetailStampMode::Height;

    // `width * height` samples per channel, planar, BORROWED. One channel for
    // Weight and Height, three for Vector.
    const float* image = nullptr;
    int width = 0;
    int height = 0;

    // World units per unit of sampled value. Signed, so a map deposits or digs
    // without a second image.
    float amplitude = 1.0f;
    // What a Height map's zero is. A map cut out of a photograph sits around
    // 0.5 and a map authored as a displacement sits around 0, and guessing
    // wrong inflates or deflates the whole stamp.
    float bias = 0.0f;

    // The square, with `MeshBrushSettings`' meanings: the plane through
    // `center` whose normal is `direction`, oriented by `tangent` (any rough
    // "up" works, it is re-orthogonalised), of side `extent`.
    kernel::cfloat3 center = kernel::cf3(0, 0, 0);
    kernel::cfloat3 direction = kernel::cf3(0, 0, 0);  // 0 = the caller's fallback
    kernel::cfloat3 tangent = kernel::cf3(0, 0, 0);    // 0 = derived
    float extent = 1.0f;

    bool valid() const {
        return image != nullptr && width >= 2 && height >= 2 && extent > 0.0f;
    }
    int channels() const { return mode == DetailStampMode::Vector ? 3 : 1; }
};

// The stamp's placement, resolved once per stamp rather than per vertex. The
// same shape and the same construction as `AlphaFrame`, built by the same
// `kernel::calpha_frame`.
AlphaFrame detail_stamp_frame(const DetailStampSettings& settings,
                              kernel::cfloat3 fallback_direction);

// What one vertex reads out of the stamp, already in the vertex's own frame.
//
// `weight` is the Weight-mode answer and is 1 in the other modes, so a caller
// multiplies unconditionally; `offset` is the displacement in frame
// coefficients and is zero in Weight mode, for the same reason.
struct DetailStampSample {
    float weight = 1.0f;
    LocalDetail offset;
    // Whether the point fell inside the square. `calpha_sample` CLAMPS to the
    // border outside it, which is right for a scalar alpha whose radial weight
    // ends the influence anyway — and wrong for a displacement, where a clamped
    // border would smear the map's edge row across everything the brush
    // reaches. A caller drops what is outside.
    bool inside = false;
};

// Sample the stamp for one vertex. `frame` is the vertex's transported frame,
// which is what makes a Vector stamp follow the surface; `position` is where
// the vertex is, in the same space the placement was given in.
DetailStampSample detail_stamp_sample(const DetailStampSettings& settings, const AlphaFrame& frame,
                                      const SurfaceFrame& vertex_frame,
                                      kernel::cfloat3 position);

// WHETHER THE LEVEL CAN CARRY WHAT THE STAMP HOLDS, reported rather than
// smoothed over.
//
// A displacement map at 2048 samples across a 5 mm square carries features
// finer than a level whose mean edge is 1 mm can represent, and applying it
// anyway produces a surface that looks like the map through a blur — which
// reads as a bug in the map, or in the brush, or in the artist's file, and is
// none of those. The library that implied the resolution should be the one that
// says it does not have it.
struct DetailStampReport {
    // The world size of one image sample over the placed square.
    float sample_size = 0.0f;
    // The level's mean edge length, which is what a vertex spacing amounts to.
    float vertex_spacing = 0.0f;
    // Samples per vertex spacing. Above 1 the map carries detail the level
    // cannot hold.
    float oversampling = 0.0f;
    bool under_resolved = false;
};

DetailStampReport detail_stamp_report(const DetailStampSettings& settings, float vertex_spacing);

}  // namespace mesh
}  // namespace clay
