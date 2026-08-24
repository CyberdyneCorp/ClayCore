#pragma once

// MEASURING THE SURFACE AT A POINT.
//
// `mask_from_surface` derived curvature, cavity and convexity from the field's
// Laplacian and could only ever return a LATTICE. That is the right shape for
// masking and the wrong one for everything else: a baker sampling a texture
// wants a value per point, a host colouring a viewport wants one per vertex, and
// neither wants to build a MaskField and read it back.
//
// So the measure moves here and `mask_from_surface` becomes one of its callers.
// That is not tidying — it is what makes "the mask and the baked map agree
// about this surface" a construction rather than a claim, since there is one
// implementation and no second stencil to drift.
//
// AMBIENT OCCLUSION AND THICKNESS join them here, and they were deliberately
// absent before. `procedural_mask.h` said why: "both need rays cast from the
// surface, which is a different cost class and a different set of parameters
// (ray count, length, falloff), so they are their own change rather than two
// more enumerators here pretending to be as cheap as the rest." They are those
// parameters now, stated rather than assumed.
//
// WHY THEY ARE STILL CHEAPER HERE than in a mesh engine, which is the same
// argument the Laplacian made. A mesh has to trace rays against triangles
// through an acceleration structure that must be rebuilt when the mesh changes.
// A field is ray-marched directly, at any resolution, with no structure to
// build and nothing to invalidate — so an AO sample costs a march rather than a
// traversal, and it measures the ACTUAL surface rather than a tessellation of
// it.
//
// DETERMINISM IS NOT NEGOTIABLE HERE. Every other query in this library returns
// the same bits on every backend and every run, and a hemisphere sample is the
// first thing in the tree that could quietly break that. So the sample pattern
// is a fixed low-discrepancy sequence rotated by a hash of the POINT and an
// explicit seed — not a random number generator, not a thread-dependent
// sequence, and not something that changes with ray order. Two runs with the
// same seed give the same bits, and so do two backends.

#include <cstdint>
#include <functional>

#include "clay/kernel/shim.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace brush {

enum class SurfaceMeasure {
    // |Laplacian|: anywhere the surface bends, in either direction. The one to
    // reach for when the intent is "the detail", not "the crevices".
    Curvature,
    // Concave only — crevices, the inside of a fold, the seam where two forms
    // meet. ZBrush's cavity masking.
    Cavity,
    // Convex only — hard edges, ridges, the outside of a fold. The mask a
    // polish or a trim wants.
    Convexity,
    // How closely the surface normal agrees with a direction. "Everything
    // facing up", for wear, dust and gravity-driven effects.
    NormalDirection,
    // How enclosed a point is: the fraction of a hemisphere around the normal
    // that is blocked within `ray_length`. 0 is open sky, 1 is fully enclosed.
    //
    // OCCLUSION, not "lighting" — the greater number is the darker place. Tools
    // disagree about which way this runs, so the name says which.
    AmbientOcclusion,
    // How much material is behind the surface: the distance from the point
    // INWARD along -normal to where the field turns positive again, normalised
    // by `ray_length`. 1 is thicker than the probe could measure, 0 is a
    // vanishingly thin shell.
    //
    // Ambient occlusion's inward twin, sharing its machinery: one asks what is
    // in front, the other what is behind.
    Thickness,
};

struct MeasureSettings {
    // The finite-difference step, in world units, for the Laplacian and the
    // normal. 0 derives one from `scale`. Smaller measures noise; larger blurs
    // the feature being measured.
    float h = 0.0f;

    // -- curvature, cavity, convexity ----------------------------------------
    // The radius that reads as fully saturated. Curvature is 1/radius, so 0.05
    // means a 5 cm fillet reads as 1.0 and a gentler one reads lower.
    float scale = 0.05f;

    // -- normal direction ----------------------------------------------------
    kernel::cfloat3 direction = kernel::cf3(0.0f, 1.0f, 0.0f);  // need not be unit
    float threshold = 0.0f;  // the dot product below which a point reads zero

    // -- ambient occlusion and thickness -------------------------------------
    // How far a probe looks. THIS IS THE PARAMETER THAT DECIDES WHAT THE NUMBER
    // MEANS: occlusion measured over 1 cm and over 1 m describe different
    // things about the same point, and neither is more correct. There is no
    // good default, so a caller that leaves it at 0 gets 20x `scale` and the
    // interface says that is a guess.
    float ray_length = 0.0f;
    // Rays per hemisphere. Cost is linear in this and the noise falls as its
    // square root, so doubling quality costs four times. Thickness ignores it:
    // it is one ray.
    int ray_count = 16;
    // Occlusion is weighted by 1/(1 + falloff * t), so a near blocker counts
    // for more than a far one. 0 makes every hit count the same.
    float falloff = 1.0f;
    // The sample pattern's seed. Two calls with the same seed give the same
    // bits, on every backend and every run — see the note above.
    std::uint32_t seed = 0;
};

// One measure at one point. `f` is the signed distance field.
//
// The point is taken as given and is NOT projected onto the surface first: a
// caller sampling a mesh's vertices already has surface points, and one
// sampling a lattice deliberately wants the value where it asked. A measure
// taken deep inside a solid describes nothing an artist can see, which is why
// `mask_from_surface` bands its region — the banding is the caller's decision,
// not this function's.
float measure_at(const std::function<float(kernel::cfloat3)>& f, SurfaceMeasure measure,
                 kernel::cfloat3 p, const MeasureSettings& settings);

// The same, over many points. Parallel across points and cancellable, because
// a bake is millions of them and an AO sample is a march per ray.
//
// A cancelled call leaves `out_values` UNSPECIFIED and reports it: a partially
// written buffer read as if complete is a texture with a band of garbage in it,
// and that is worse than no texture.
void measure_points(const std::function<float(kernel::cfloat3)>& f, SurfaceMeasure measure,
                    const kernel::cfloat3* points, std::size_t count,
                    const MeasureSettings& settings, float* out_values,
                    parallel::CancelToken* token = nullptr, bool* out_cancelled = nullptr);

}  // namespace brush
}  // namespace clay
