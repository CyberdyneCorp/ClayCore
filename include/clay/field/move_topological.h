#pragma once

// Move Topological (sdf-kernels spec, add-move-topological).
//
// `grab` weights its falloff by |p - centre| / radius — Euclidean distance
// through SPACE. This weights it by distance along the MATERIAL, so pulling one
// finger does not drag the finger beside it.
//
// Measured on two fingers 0.32 apart, joined only through a palm below them: a
// Euclidean drag at radius 0.50 pulls the neighbouring finger's near edge from
// +0.158 to +0.074, while geodesically it is about 1.5 away — down one finger,
// across the palm and up the other. Euclidean distance cannot tell those apart,
// and that difference is the entire brush.
//
// It BAKES, for the reason relax and flatten do. The weight is a solved grid
// rather than a closed form, and putting one in the tape would mean a deformer
// that reads out-of-line data — which no deformer does, and which every backend
// would have to grow. Baking needs no kernel change.

#include <cstdint>
#include <cstddef>
#include <functional>

#include "clay/field/volume.h"

namespace clay {
namespace field {

struct TopologicalMoveSettings {
    // A point on or near the surface being dragged — what a pick already gives.
    kernel::cfloat3 anchor = kernel::cf3(0, 0, 0);

    // The reach, in world units measured ALONG THE MATERIAL. This is the one
    // parameter that means something different from `grab`'s: a radius of 0.5
    // here reaches half a unit of travel across the surface, not half a unit of
    // straight line, so it cannot step over a gap however narrow.
    float radius = 0.3f;

    kernel::cfloat3 displacement = kernel::cf3(0, 0, 0);
    std::uint8_t ease = 0;
};

// Sample `source` with a topological move applied.
//
// The geodesic distance is solved on a local grid at `cell_size`, over the cells
// the source reports as material. Free space is not part of the graph, which is
// what stops the weight crossing a gap; the solved field is then dilated a few
// cells outward, because the warp acts on space near the surface and a point
// just outside it still needs a weight.
//
// The result declares the Lipschitz its samples actually have, measured rather
// than bounded in advance — a weight that varies along the surface can steepen
// the field, and by how much depends on the form.
//
// A zero displacement, a radius that is not positive, or an anchor with no
// material within reach all yield a plain sampling of the source: a drag that
// touches nothing is not an error.
//
// A DRAG LONGER THAN THE REACH IS SUB-STEPPED (#657). One step pulls each
// output point back by p - d * w(g(p)), and that map stops being one-to-one
// once |d| * slope / radius passes one: two output points read one source
// point, and the surface under the grip sinks while the pulled material falls
// away before the drag's end. Measured on a unit sphere, a 0.64 drag at radius
// 0.3 left the anchor at 0.940 against 1.000. So the drag runs as
// topological_move_steps() slices of d/n, each anchored where the one before
// left the grip and each solving its own geodesic over the material the
// earlier ones left -- what a host calling n times would get, to within 0.003
// on that probe -- while the source is still read ONCE per output sample, at
// the composed pull-back. A drag under half the radius on a linear curve is
// one step and identical to what this did before.
// A source that answers a batch of arbitrary points at once: `count` packed
// xyz triples in, `count` distances out.
using PointBatch = std::function<void(const float* points_xyz, std::size_t count, float* out)>;

FieldVolume move_topological(const std::function<float(kernel::cfloat3)>& source,
                             const math::Aabb& region, float cell_size, float band,
                             const TopologicalMoveSettings& settings);

// The same, with a volume as the source — which is what an imported mesh gives,
// since there is no document behind it. The geodesic solve only needs each cell's
// SIGN, which a narrow band reports correctly either side of the surface, so this
// is less lossy here than it is for flatten. The result is still sampled over the
// volume's own region.
// The same again, from a source that answers a BATCH of arbitrary points --
// packed xyz in, distances out, the same shape as FieldVolume::ColorBlockFill.
//
// NOT a BrickBlockFill, and the difference is the whole reason this overload
// has a type of its own. Relax and flatten evaluate their source AT the sample
// lattice, so a fill that knows the grid can answer them. This one samples at a
// PULLED-BACK point: where an output sample takes its material from depends on
// the geodesic weight there, so the query positions are not the lattice and
// only the caller's evaluator can be told where they are.
//
// A tape is the source this exists for, and both places the source is asked
// anything go through it: the material array the geodesic walk runs on, which
// is one dense box of cells, and the sampling pass itself. Evaluating a
// document costs about ten nanoseconds per instruction against one nanosecond
// of arithmetic, so the interpreter is nearly all of this operation, and it was
// being paid one point at a time. `eval::tape_point_batch` is that evaluator
// for a document.
//
// Identical to the overload above given a source that agrees with the callable.
FieldVolume move_topological(const PointBatch& source, const math::Aabb& region,
                             float cell_size, float band,
                             const TopologicalMoveSettings& settings);

// Takes bounds, cell size, band AND feather from the source volume, so a
// result can be composited on the same terms the source was. The free
// functions above cannot: they are handed a region rather than a volume, and
// leave the feather at its default.
FieldVolume move_topological(const FieldVolume& v, const TopologicalMoveSettings& settings);

// How many slices move_topological splits this drag into: the fewest that keep
// each slice's |d/n| * ease_max_slope / radius at or under one half, capped at
// 64. Past the cap -- a drag over thirty radii on a linear curve -- the slices
// are longer than that and the map can fold again; the cap bounds the work a
// single call can be asked for rather than refusing the drag. 1 for a radius
// that is not positive, where nothing is moved anyway.
int topological_move_steps(const TopologicalMoveSettings& settings);

}  // namespace field
}  // namespace clay
