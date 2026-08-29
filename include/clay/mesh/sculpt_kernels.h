#pragma once

// The deformation math behind the mesh verbs, with no representation attached
// (meshing spec, add-shared-brush-kernels).
//
// THE RULE THIS HEADER IS: nothing here may name a `Mesh`, an `Adjacency`, a
// `Bvh` or a vertex index. A kernel takes a snapshot of what is under the
// brush — positions, normals, weights, a neighbourhood, a frame and a plane —
// and writes one displacement per entry. Everything about WHERE those numbers
// came from and where they go back belongs to the sculptor that called it.
//
// WHY THAT RULE. Three sculptors want this math: the fixed-topology one, the
// adaptive one and the multiresolution one. If it stays interleaved with the
// weld-class walk that feeds it, the second and third can only have it by
// copying it — and the moment Clay is a copy, Clay means two things. An artist
// who learns a brush on a mesh layer and finds it behaves differently on an
// adaptive one has not found a bug they can report; they have found that the
// tool is untrustworthy.
//
// The neighbourhood arrives as CSR rather than as a callback. A callback per
// neighbour is a virtual call in the innermost loop of a smoothing pass, and
// the same loop has to run on a million-vertex surface at pointer rates. The
// sculptor already walks its own adjacency to build a region; writing that walk
// into two flat arrays costs the walk it was doing anyway.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/brush_model.h"
#include "clay/mesh/sculpt_common.h"

namespace clay {
namespace mesh {

// A neighbour that is not itself under the brush. The kernels read such a
// neighbour from the surface rather than from the buffer they are iterating,
// which is what keeps a smoothing pass from smoothing against the rim of its
// own region.
inline constexpr std::uint32_t kOutsideRegion = 0xffffffffu;

// -- the weight ---------------------------------------------------------------

// The falloff curve over the normalized distance from the brush centre.
// Deliberately identical to `voxel::falloff_weight`, curve for curve and
// constant for constant, so a brush behaves the same on both representations;
// the duplication is the module layering and is explained in `sculpt_common.h`.
float falloff_weight(MeshFalloff curve, float d);

// The per-vertex weight, composed in ONE fixed order.
//
// THE ORDER IS PART OF THE CONTRACT, not an implementation detail. These are
// separate multiplications and float multiplication is not associative, so
// re-associating them moves the last bit of the weight and therefore of every
// displacement that reads it:
//
//     falloff -> path taper -> (1 - gate) -> alpha -> automask
//
// An automask factor is applied LAST specifically so that a stamp with none of
// them reproduces the pre-extraction bits exactly. Multiplying by an identical
// 1.0 is the identity in IEEE-754, but multiplying it in earlier would
// re-associate the four factors that were already there.
//
// The clamp on the gate is a domain guard on a caller-supplied callback and
// stays where it is: a gate returning 1.2 must freeze, and an unclamped
// `1 - 1.2` would only reach that answer by being dropped at the boundary,
// while a gate returning -0.5 would AMPLIFY the weight instead of leaving it
// alone.
struct WeightFactors {
    float falloff = 1.0f;
    float path_taper = 1.0f;  // geodesic walks only; 1 elsewhere
    float gate = 0.0f;        // the freeze, in [0,1] after its own clamp
    float alpha = 1.0f;       // the borrowed stamp
    float automask = 1.0f;    // composed automask factors
};

float compose_weight(const WeightFactors& f);

// The smoothstep fade over the last stretch of a geodesic walk's path budget,
// which keeps the region's rim smooth where the straight-line and along-surface
// distances disagree. `path` is the walk's own distance in radius units.
//
// The two bounds are PARAMETERS rather than defaults read from a constant here,
// and that is the layering rather than a preference: they are the walk's
// policy, they live beside the walk in `adjacency.h`, and this header may not
// name that file. A representation with no surface walk passes 1 and pays
// nothing.
float path_taper(float path, float taper_start, float budget);

// The stamp's frame for an alpha, derived once per stamp. Neutral because the
// alpha is part of the WEIGHT rather than part of a verb: it multiplies what
// every kernel already scales by, which is why an alpha needs no per-verb code
// on any representation.
//
// The frame is built by `kernel::calpha_frame`, the same function the SDF alpha
// uses, so one stamp reads identically on a mesh and on a field rather than
// through two bilinear lookups that could drift apart.
struct AlphaFrame {
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    kernel::cfloat3 tangent = kernel::cf3(1, 0, 0);
    kernel::cfloat3 binormal = kernel::cf3(0, 1, 0);
    float extent = 1.0f;
};

// Build the frame from the settings and a fallback direction, used when the
// caller supplied none. The fallback is the sculptor's business because it is
// the one part that reads a surface.
AlphaFrame alpha_frame_for(const MeshBrushSettings&, kernel::cfloat3 fallback_direction);

// The alpha's value at a world point, or 1 where there is no alpha — so a
// caller multiplies unconditionally and an absent stamp is exactly the weight
// it had, rather than a branch per vertex.
float alpha_at(const MeshBrushSettings&, const AlphaFrame&, kernel::cfloat3 p);

// -- the snapshot -------------------------------------------------------------

// Everything one stamp READS, captured before anything is written. A composed
// verb is one operation against one snapshot rather than a sequence of calls:
// Crease is a draw AND a pinch against these positions, Scrape is a cut AND a
// smooth against them, and calling the halves in sequence is a different
// operation and a worse one.
struct SculptSnapshot {
    const kernel::cfloat3* positions = nullptr;  // pre-stamp, one per entry
    const kernel::cfloat3* normals = nullptr;    // pre-stamp, unit
    const float* weights = nullptr;              // composed, in [0,1]
    std::size_t count = 0;

    // The stamp's frame, taken from the snapshot and never from what the stamp
    // is about to deposit.
    kernel::cfloat3 average_normal = kernel::cf3(0, 1, 0);
    kernel::cfloat3 centroid = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    std::size_t size() const { return count; }
};

// The one-ring of every snapshot entry, flattened. `offsets` has `count + 1`
// entries; the neighbours of entry `i` are `[offsets[i], offsets[i + 1])`.
//
// `slots[k]` is the neighbour's index in the snapshot, or `kOutsideRegion` when
// the neighbour is not under the brush. The parallel arrays carry what an
// outside neighbour cannot be read from the snapshot for; they are filled per
// neighbour whether or not it is inside, because the cost is the walk and the
// branch is not worth the asymmetry.
//
// `normals` is filled ONLY for the verbs that read it — polish is the only one
// — because a geometric normal per neighbour costs a pass over every
// neighbour's own ring, and no other verb would use it.
struct SculptNeighbors {
    const std::uint32_t* offsets = nullptr;
    const std::uint32_t* slots = nullptr;
    const kernel::cfloat3* positions = nullptr;
    const kernel::cfloat3* normals = nullptr;  // polish only; may be null
    const kernel::cfloat3* colors = nullptr;   // smear only; may be null

    bool empty() const { return offsets == nullptr; }
};

// The buffers a multi-pass kernel iterates. Owned by the sculptor and reset
// rather than freed between stamps, so a stroke of similar stamps allocates on
// its first one and never again.
struct SculptScratch {
    std::vector<kernel::cfloat3> smoothed, smooth_tmp;
    std::vector<float> gate, gate_tmp;
};

// -- the kernels --------------------------------------------------------------
//
// Each writes one displacement per snapshot entry into `out`, which the caller
// has sized to `snapshot.count`. None of them reads or writes a surface.

void kernel_grab(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);

// ONE deformation under two frames, which is what draw and inflate always
// were: the region's averaged normal gives a rounded organic swell, each
// vertex's own normal gives a balloon, and nothing else about them differed.
// `kernel_draw` and `kernel_inflate` remain as the two named readings, and
// their results are unchanged bit for bit.
void kernel_displace(const SculptSnapshot&, const MeshBrushSettings&, BrushFrame frame,
                     kernel::cfloat3* out);
inline void kernel_draw(const SculptSnapshot& s, const MeshBrushSettings& settings,
                        kernel::cfloat3* out) {
    kernel_displace(s, settings, BrushFrame::RegionNormal, out);
}
inline void kernel_inflate(const SculptSnapshot& s, const MeshBrushSettings& settings,
                           kernel::cfloat3* out) {
    kernel_displace(s, settings, BrushFrame::VertexNormal, out);
}
void kernel_pinch(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);
void kernel_flatten(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);
void kernel_clay(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);
void kernel_crease(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);
void kernel_nudge(const SculptSnapshot&, const MeshBrushSettings&, kernel::cfloat3* out);

// Smooth, Polish and Scrape: one Laplacian target, three readings of it.
void kernel_smooth_family(MeshBrush verb, const SculptSnapshot&, const SculptNeighbors&,
                          const MeshBrushSettings&, SculptScratch&, kernel::cfloat3* out);

// Relax takes the SAME Laplacian target smooth uses and removes its normal
// component, so the two cannot disagree about what the neighbourhood average
// is.
void kernel_relax(const SculptSnapshot&, const SculptNeighbors&, const MeshBrushSettings&,
                  SculptScratch&, kernel::cfloat3* out);

// Layer measures against `origin` — where the surface was when the STROKE
// began, one per entry — rather than against where it is now, which is what
// makes a slow stroke and a fast one over the same path agree.
void kernel_layer(const SculptSnapshot&, const MeshBrushSettings&,
                  const kernel::cfloat3* origin, kernel::cfloat3* out);

// The colour pair. A colour write is a kernel with a different write target,
// not an exception to the model: `current` and `out` are colours rather than
// displacements, and neither kernel moves a vertex.
void kernel_paint(const SculptSnapshot&, const MeshBrushSettings&,
                  const kernel::cfloat3* current, kernel::cfloat3* out);
void kernel_smear(const SculptSnapshot&, const SculptNeighbors&, const MeshBrushSettings&,
                  const kernel::cfloat3* current, kernel::cfloat3* out);

// -- shared geometry ----------------------------------------------------------
//
// Exposed because the sculptors need them to BUILD a snapshot, not only to
// consume one, and a second copy would be a second answer.

kernel::cfloat3 safe_normalize(kernel::cfloat3 v, kernel::cfloat3 fallback);
bool is_zero(kernel::cfloat3 v);
// The part of `v` lying in the surface: what makes a pinch gather ALONG the
// surface instead of sinking the region into it.
kernel::cfloat3 tangential(kernel::cfloat3 v, kernel::cfloat3 n);
// The move onto a plane, clamped by the mode. CutOnly removes material above
// the plane and leaves the hollows below it, which is the whole of Trim Dynamic
// and hPolish; FillOnly is the dual.
kernel::cfloat3 plane_offset(kernel::cfloat3 p, kernel::cfloat3 point, kernel::cfloat3 normal,
                             field::FlattenMode mode);
// How far a surface BENDS at a vertex: the mean angle between its normal and
// its neighbours'. Polish's gate reads it, and it takes neighbour normals
// rather than face normals for the reason `kernel_smooth_family` records.
float mean_ring_disagreement(const kernel::cfloat3* neighbor_normals, std::size_t count,
                             kernel::cfloat3 n);

}  // namespace mesh
}  // namespace clay
