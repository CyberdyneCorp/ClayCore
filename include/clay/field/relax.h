#pragma once

// Relaxing a sampled field (sdf-kernels spec, add-sdf-relax).
//
// The last ZBrush core brush: voxels smooth, SDF layers had nothing. Smoothing
// happens by SAMPLING the field, averaging, and re-sampling — the field-space
// re-blend alternative would have made an edit list mean "shapes plus a rule
// about how they interact", which reaches undo, picking, serialization and the
// C ABI, and still could not smooth a bump in the middle of a single item.
//
// Convolving a distance field destroys EXACTNESS: the result no longer reports
// the true distance to its own surface. It does NOT break the Lipschitz bound,
// and that is the property the evaluator actually depends on. Averaging can
// only shrink the bound — for weights summing to one,
//
//     |f(x) - f(y)| <= sum_i w_i |f(x + d_i) - f(y + d_i)| <= |x - y|
//
// and a 1-Lipschitz field is automatically a conservative bound on the distance
// to its own zero set: if z is the nearest zero to x then
// |f(x)| = |f(x) - f(z)| <= |x - z|. So sphere tracing on a relaxed field
// cannot overstep, which is what makes this small rather than large.
//
// Relax BAKES. What comes back is a volume, not the edit list that went in.
// That is inherent to relaxing a field, not a shortcut taken here.

#include <functional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace field {

// An optional freeze, taken as a CALLABLE rather than as a mask type. A sampled
// field is a leaf — it sits below `scene`, while a mask sits above it — so
// naming voxel::MaskField here would make field -> voxel -> scene -> field a
// cycle. What a verb actually needs from a mask is a scalar at a world point,
// and that is what this is: 0 lets an edit through, 1 freezes it. Empty means
// no mask, which costs nothing.
using MaskGate = std::function<float(kernel::cfloat3)>;

struct RelaxSettings {
    // How much of the smoothed value to take, per iteration. 1 replaces the
    // field with the average; 0 changes nothing.
    float strength = 1.0f;

    // The averaging radius, in cells. Larger smooths coarser features. The
    // taps are the cell-aligned neighbourhood within this radius.
    int radius_cells = 1;

    // More passes of a small kernel is not the same shape as one pass of a
    // large one: a small kernel repeated approaches a Gaussian, a large one
    // stays boxy.
    int iterations = 1;

    // Where it acts. A radius of zero means everywhere, which is a filter
    // rather than a brush.
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    float region_radius = 0.0f;
    // Over what distance the effect tapers to nothing at the region's edge.
    // Zero would leave a visible rim; it is clamped to something that will not.
    float falloff = 0.0f;

    // Optional freeze, exactly as the voxel verbs take one: the weight at a
    // sample is scaled by (1 - mask) at that sample's WORLD position, so a
    // fully masked sample keeps its value verbatim. Sampling in world units
    // rather than in this volume's cells costs nothing and is the whole reason
    // the mask lattice is addressed that way — one mask can freeze a voxel
    // layer and an SDF layer at once.
    MaskGate mask;
};

// What one relax touched, for a caller holding a preview of the volume it
// relaxed. See FieldVolume::RewriteTally for why the bounds and the count are
// geometric while `changed` is not.
struct RelaxResult {
    math::Aabb dirty_bounds;
    std::size_t touched_bricks = 0;
    bool changed = false;
    // Set when `token` fired. The checkpoint is the PASS boundary, so a
    // cancelled call has applied some number of whole passes and no fraction
    // of one — there is no half-written state to inspect or to discard.
    bool cancelled = false;
};

// Smooth `volume` IN PLACE.
//
// The same algorithm as `relax` below with different ownership, and that is
// the whole of the difference. `relax` begins by copying its input, which is
// right for a standalone operation and wrong for a live gesture: a Smooth
// stroke already owns a private working volume, so building a second complete
// result object per dab puts a term that scales with the MODEL back into a dab
// that was made to scale with itself.
//
// CANCELLATION cannot mean "hand back the input" here, because there is no
// input to hand back — the caller's volume IS the working state. What it means
// instead is the strongest thing an in-place operator can promise: the check
// sits between whole passes, so every pass is applied entirely or not at all,
// and `cancelled` says a later pass was not run. A live transaction keeps the
// passes it paid for; a standalone `relax` discards the lot, which is what it
// has always done.
//
// The band is narrowed by what the COMPLETED passes could have moved the
// surface, not by what all of them would have: a cancelled relax that shrank
// the band for work it never did would understate the distance a sample-free
// brick reports, which is the one direction a bound may not be wrong in.
// `out_changed` (optional) is APPENDED with the coordinate of every brick in
// which a stored sample actually moved, so a caller transporting a preview
// sends the bytes that are new rather than the whole volume. The vector is the
// caller's and is reused across dabs; see FieldVolume::rewrite_region_tallied.
RelaxResult relax_in_place(FieldVolume& volume, const RelaxSettings& settings = {},
                           parallel::CancelToken* token = nullptr,
                           std::vector<FieldVolume::BrickCoord>* out_changed = nullptr);

// Smooth `v`, returning a new volume sampled over the same region at the same
// resolution. The input is not modified.
FieldVolume relax(const FieldVolume& v, const RelaxSettings& settings = {},
                  parallel::CancelToken* token = nullptr);

// The same, sampled from an arbitrary SOURCE first — a document's tape is the
// intended one — mirroring flatten's pair of overloads. Deliberately exactly
// sample-then-relax: relax averages cell-aligned taps, and the taps of a
// fresh bake ARE the source at those lattice points, so there is nothing a
// fused form could do better. What the overload buys is one entry point for
// hosts, and a source that is exact rather than a volume carrying a band.
FieldVolume relax(const std::function<float(kernel::cfloat3)>& source,
                  const math::Aabb& region, float cell_size, float band,
                  const RelaxSettings& settings = {});

// The same again, from a source that fills whole BLOCKS of the sample lattice
// rather than answering one point at a time.
//
// A tape is the source this exists for. Evaluating a document costs about ten
// nanoseconds per instruction and one nanosecond of arithmetic, so the
// interpreter is most of a bake and the interpreter is per point; handing an
// evaluator a window of points lets it compile the tape once and spread the
// window across a pool. `eval::tape_block_fill` is that fill for a document,
// and `scene::bake_layer` has gone through the same door since it was written.
//
// Byte-identical to the overload above given a fill that agrees with the
// callable — which is the contract `sample_blocks` already states, since blocks
// land in slot order whatever order they were computed in.
FieldVolume relax(const FieldVolume::BrickBlockFill& source,
                  const math::Aabb& region, float cell_size, float band,
                  const RelaxSettings& settings = {});

}  // namespace field
}  // namespace clay
