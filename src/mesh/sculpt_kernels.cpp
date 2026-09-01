#include "clay/mesh/sculpt_kernels.h"

#include <algorithm>
#include <cmath>

#include "clay/kernel/deform.h"  // calpha_sample, calpha_frame
#include "clay/mesh/stamp_frame.h"

namespace clay {
namespace mesh {

// -- the weight ---------------------------------------------------------------

float falloff_weight(MeshFalloff curve, float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    switch (curve) {
        case MeshFalloff::Linear:
            return 1.0f - d;
        case MeshFalloff::Smooth: {
            const float t = 1.0f - d;
            return t * t * (3.0f - 2.0f * t);
        }
        case MeshFalloff::Gaussian:
            return std::exp(-4.5f * d * d);
        case MeshFalloff::Constant:
        default:
            return 1.0f;
    }
}

float path_taper(float path, float taper_start, float budget) {
    const float over = (path - taper_start) / (budget - taper_start);
    if (over <= 0.0f) return 1.0f;
    const float t = std::clamp(over, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The order below is the contract; see the header. Written as successive
// compound multiplications rather than one product expression, because a single
// expression lets the compiler re-associate and that is exactly the freedom
// this must not have.
float compose_weight(const WeightFactors& f) {
    float w = f.falloff;
    w *= f.path_taper;
    w *= 1.0f - std::clamp(f.gate, 0.0f, 1.0f);
    w *= f.alpha;
    w *= f.automask;
    return w;
}

// THE ALPHA'S FRAME IS ONE READING OF THE STAMP'S FRAME, and since
// add-shared-brush-runtime it is literally that: `make_stamp_frame` reaches
// `kernel::calpha_frame` exactly once with the same two arguments this used to
// pass it, so an unrotated alpha lands on the bits it landed on before the
// stamp frame existed — by construction rather than by measurement.
//
// `AlphaFrame` itself stays. It is the CACHED record `alpha_at` reads, and it
// carries `extent`, which is a property of the stamp rather than of the basis.
AlphaFrame alpha_frame_for(const MeshBrushSettings& settings, kernel::cfloat3 fallback_direction) {
    AlphaFrame frame;
    if (!settings.has_alpha()) return frame;
    frame.centre = settings.center;
    frame.extent = settings.alpha_extent > 0.0f ? settings.alpha_extent : 2.0f * settings.radius;
    kernel::cfloat3 dir = settings.alpha_direction;
    if (kernel::clength(dir) < 1e-9f) dir = fallback_direction;
    const StampFrame stamp =
        make_stamp_frame(settings.center, dir, settings.alpha_tangent, settings.stamp_azimuth);
    frame.tangent = stamp.tangent;
    frame.binormal = stamp.bitangent;
    return frame;
}

float alpha_at(const MeshBrushSettings& settings, const AlphaFrame& f, kernel::cfloat3 p) {
    if (!settings.has_alpha()) return 1.0f;
    const kernel::cfloat3 rel = p - f.centre;
    const float u = kernel::cdot(rel, f.tangent) / f.extent + 0.5f;
    const float v = kernel::cdot(rel, f.binormal) / f.extent + 0.5f;
    return kernel::calpha_sample(settings.alpha, settings.alpha_width, settings.alpha_height, u, v);
}

// -- shared geometry ----------------------------------------------------------

bool is_zero(kernel::cfloat3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

kernel::cfloat3 safe_normalize(kernel::cfloat3 v, kernel::cfloat3 fallback) {
    const float len = kernel::clength(v);
    return len > 1e-20f ? v / len : fallback;
}

kernel::cfloat3 tangential(kernel::cfloat3 v, kernel::cfloat3 n) {
    return v - n * kernel::cdot(v, n);
}

kernel::cfloat3 plane_offset(kernel::cfloat3 p, kernel::cfloat3 point, kernel::cfloat3 normal,
                             field::FlattenMode mode) {
    const float d = kernel::cdot(p - point, normal);
    if (mode == field::FlattenMode::CutOnly && d <= 0.0f) return kernel::cf3(0, 0, 0);
    if (mode == field::FlattenMode::FillOnly && d >= 0.0f) return kernel::cf3(0, 0, 0);
    return normal * -d;
}

// Two choices here, both made against the surface polish is actually for — a
// noisy flat beside a hard edge — and both wrong in the obvious reading.
//
// NEIGHBOURING VERTEX normals rather than incident FACE normals. "Dihedral
// angle" says faces, and on a noisy surface the faces are useless: noise tilts
// each one by more than a chamfer bends the whole surface, so every flat looks
// like an edge and polish declines to do anything at all. A vertex normal is
// already an angle-weighted average over its one-ring of faces, so most of the
// noise is out of it and what is left is the shape — which is what the gate
// means to read.
//
// The MEAN rather than the widest. The widest is one sample out of six and
// carries the residual noise straight back in; the mean is the statistic that
// says "how much does the surface bend here", which is the question.
float mean_ring_disagreement(const kernel::cfloat3* neighbor_normals, std::size_t count,
                             kernel::cfloat3 n) {
    if (count == 0) return 0.0f;
    float total = 0.0f;
    for (std::size_t i = 0; i < count; ++i)
        total += std::acos(std::clamp(kernel::cdot(neighbor_normals[i], n), -1.0f, 1.0f));
    return total / static_cast<float>(count);
}

namespace {

// The deposit direction one stamp shares. Zero — the default — means "the
// region's averaged normal", which is what makes Draw a rounded organic swell
// instead of a balloon.
kernel::cfloat3 deposit_direction(const SculptSnapshot& s, const MeshBrushSettings& settings) {
    return is_zero(settings.deposit_normal)
               ? s.average_normal
               : safe_normalize(settings.deposit_normal, s.average_normal);
}

// One Laplacian pass: the one-ring mean, read from `current` where the
// neighbour is inside the region and from the surface where it is not. Reading
// a snapshot rather than the buffer being written is what makes this a
// simultaneous average instead of a Gauss-Seidel sweep whose result depends on
// vertex order.
void laplacian_pass(const SculptSnapshot& s, const SculptNeighbors& nb,
                    const std::vector<kernel::cfloat3>& current,
                    std::vector<kernel::cfloat3>* out) {
    for (std::size_t i = 0; i < s.count; ++i) {
        const std::uint32_t begin = nb.offsets[i], end = nb.offsets[i + 1];
        const std::size_t n = end - begin;
        if (n == 0) {
            (*out)[i] = current[i];
            continue;
        }
        kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
        for (std::uint32_t k = begin; k < end; ++k) {
            const std::uint32_t slot = nb.slots[k];
            sum = sum + (slot != kOutsideRegion ? current[slot] : nb.positions[k]);
        }
        (*out)[i] = sum / static_cast<float>(n);
    }
}

// The Laplacian target for every entry, `iterations` passes deep.
void smooth_targets(const SculptSnapshot& s, const SculptNeighbors& nb, int iterations,
                    std::vector<kernel::cfloat3>* result, std::vector<kernel::cfloat3>* scratch) {
    result->assign(s.positions, s.positions + s.count);
    scratch->resize(result->size());
    const int passes = std::clamp(iterations, 1, kMaxSmoothIterations);
    for (int it = 0; it < passes; ++it) {
        laplacian_pass(s, nb, *result, scratch);
        result->swap(*scratch);
    }
}

// Polish's gate, per entry: full strength where the surface around a vertex is
// near-planar, fading to zero where it bends, so noise goes and a hard edge
// stays.
//
// SPREAD, THEN FEATHERED, and the order matters.
//
// Raw, the gate steps from 1 to 0 across a single edge, and what a polish
// leaves along every feature it protected is a bead of untouched vertices
// beside a fully smoothed flat — visible in a render, and a worse artefact
// than the noise it removed.
//
// Feathering alone does not fix it, because it eats the protection: a crease
// one vertex wide has gate 0 at the crease and 1 on both sides, and averaging
// pulls it straight back up — the crease is then smoothed away, which is the
// one thing polish exists not to do. So the gate is SPREAD first (each entry
// takes the minimum over its own ring), widening the protected band to cover
// the feature's neighbours, and only then feathered.
constexpr int kPolishGateSpread = 1;
constexpr int kPolishGateFeather = 2;

void polish_gate(const SculptSnapshot& s, const SculptNeighbors& nb,
                 const MeshBrushSettings& settings, std::vector<float>* gate,
                 std::vector<float>* scratch) {
    const float a = std::max(settings.polish_angle, 1e-4f);
    gate->resize(s.count);
    scratch->resize(s.count);
    for (std::size_t i = 0; i < s.count; ++i) {
        const std::uint32_t begin = nb.offsets[i], end = nb.offsets[i + 1];
        const float angle =
            mean_ring_disagreement(nb.normals + begin, end - begin, s.normals[i]);
        (*gate)[i] = 1.0f - std::clamp((angle - a) / a, 0.0f, 1.0f);
    }
    for (int pass = 0; pass < kPolishGateSpread + kPolishGateFeather; ++pass) {
        const bool spreading = pass < kPolishGateSpread;
        for (std::size_t i = 0; i < s.count; ++i) {
            const std::uint32_t begin = nb.offsets[i], end = nb.offsets[i + 1];
            float lowest = (*gate)[i], total = (*gate)[i], count = 1.0f;
            for (std::uint32_t k = begin; k < end; ++k) {
                // A neighbour outside the region is outside the brush too;
                // taking it as ungated would smooth past the rim.
                const std::uint32_t slot = nb.slots[k];
                if (slot == kOutsideRegion) continue;
                lowest = std::min(lowest, (*gate)[slot]);
                total += (*gate)[slot];
                count += 1.0f;
            }
            (*scratch)[i] = spreading ? lowest : total / count;
        }
        gate->swap(*scratch);
    }
}

// A blend that is EXACT at both ends. mix(a, b, 1) is a + (b - a) * 1, which is
// not b in floating point, so a fully-weighted dab would leave a one-ULP seam
// along the rim of every stroke — the same trap the gated ops hit.
kernel::cfloat3 blend_color(kernel::cfloat3 a, kernel::cfloat3 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

}  // namespace

// -- the kernels --------------------------------------------------------------

void kernel_grab(const SculptSnapshot& s, const MeshBrushSettings& settings,
                 kernel::cfloat3* out) {
    for (std::size_t i = 0; i < s.count; ++i) out[i] = settings.direction * s.weights[i];
}

void kernel_displace(const SculptSnapshot& s, const MeshBrushSettings& settings,
                     BrushFrame frame, kernel::cfloat3* out) {
    const float amount = settings.strength * settings.radius;
    if (frame == BrushFrame::VertexNormal) {
        // Each vertex along its OWN normal — inflate. The per-vertex direction
        // is the entire difference from the branch below.
        for (std::size_t i = 0; i < s.count; ++i) out[i] = s.normals[i] * (amount * s.weights[i]);
        return;
    }
    // ONE shared direction for the whole stamp — draw. That is what makes it a
    // rounded organic swell rather than a balloon.
    //
    // The branch is hoisted OUT of the loop rather than tested per vertex, and
    // the two arms multiply in the same order, so both readings land on exactly
    // the bits their separate implementations produced.
    const kernel::cfloat3 dir = deposit_direction(s, settings);
    for (std::size_t i = 0; i < s.count; ++i) out[i] = dir * (amount * s.weights[i]);
}

void kernel_pinch(const SculptSnapshot& s, const MeshBrushSettings& settings,
                  kernel::cfloat3* out) {
    // ONE signed deformation: positive gathers toward the centre, negative
    // spreads. Pinch and magnify are the same deformation with one sign, as
    // they are for fields and as the voxel pair documents.
    for (std::size_t i = 0; i < s.count; ++i) {
        const kernel::cfloat3 toward = settings.center - s.positions[i];
        out[i] = tangential(toward, s.normals[i]) * (settings.strength * s.weights[i]);
    }
}

void kernel_flatten(const SculptSnapshot& s, const MeshBrushSettings& settings,
                    kernel::cfloat3* out) {
    const float st = std::clamp(settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < s.count; ++i)
        out[i] = plane_offset(s.positions[i], s.plane_point, s.plane_normal,
                              settings.flatten_mode) *
                 (st * s.weights[i]);
}

void kernel_clay(const SculptSnapshot& s, const MeshBrushSettings& settings,
                 kernel::cfloat3* out) {
    // Draw's deposit CLAMPED to a plane floating at the stamp height: material
    // is added UP TO the plane and no further, so what the stamp leaves is a
    // flat-topped strip rather than a swell that follows whatever the surface
    // was doing underneath. That is the whole of Clay, and repeating it is
    // ClayBuildup.
    //
    // Which makes it, exactly, a FILL-ONLY flatten onto a plane offset from the
    // region — the deposit direction decides which side "fill" is on, so a
    // negative strength digs to a plane below instead.
    const kernel::cfloat3 dir = deposit_direction(s, settings);
    const float height = settings.strength * settings.radius;
    const kernel::cfloat3 plane_pt = s.centroid + dir * height;
    const field::FlattenMode side =
        height >= 0.0f ? field::FlattenMode::FillOnly : field::FlattenMode::CutOnly;
    for (std::size_t i = 0; i < s.count; ++i)
        out[i] = plane_offset(s.positions[i], plane_pt, dir, side) * s.weights[i];
}

void kernel_crease(const SculptSnapshot& s, const MeshBrushSettings& settings,
                   kernel::cfloat3* out) {
    // The cut AND the squeeze, summed inside one stamp. Sequenced separately
    // they leave a rounded ditch: the pinch would gather vertices the draw had
    // already pushed down, instead of closing the fold as it forms.
    const kernel::cfloat3 dir = deposit_direction(s, settings);
    const float cut = settings.strength * settings.radius;
    const float squeeze = std::fabs(settings.strength);
    for (std::size_t i = 0; i < s.count; ++i) {
        const float w = s.weights[i] * s.weights[i];  // tighter than the curve
        const kernel::cfloat3 toward = settings.center - s.positions[i];
        out[i] = dir * (-cut * w) + tangential(toward, s.normals[i]) * (squeeze * w);
    }
}

// NUDGE — grab's tangential sibling. Grab carries the region rigidly, so a
// drag across a surface lifts material off it; this slides material ALONG the
// surface instead, which is what an artist means by nudging a feature sideways.
//
// Per-vertex tangent planes rather than the region's average normal: on a
// curved region an averaged plane pushes the rim off the surface, which is the
// artifact the verb exists to avoid.
void kernel_nudge(const SculptSnapshot& s, const MeshBrushSettings& settings,
                  kernel::cfloat3* out) {
    for (std::size_t i = 0; i < s.count; ++i)
        out[i] = tangential(settings.direction, s.normals[i]) * s.weights[i];
}

void kernel_smooth_family(MeshBrush verb, const SculptSnapshot& s, const SculptNeighbors& nb,
                          const MeshBrushSettings& settings, SculptScratch& scratch,
                          kernel::cfloat3* out) {
    smooth_targets(s, nb, settings.smooth_iterations, &scratch.smoothed, &scratch.smooth_tmp);
    if (verb == MeshBrush::Polish) polish_gate(s, nb, settings, &scratch.gate, &scratch.gate_tmp);
    const float st = std::clamp(settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < s.count; ++i) {
        const kernel::cfloat3 p = s.positions[i];
        const kernel::cfloat3 relax = scratch.smoothed[i] - p;
        const float w = s.weights[i];
        if (verb == MeshBrush::Smooth) {
            out[i] = relax * (w * st);
        } else if (verb == MeshBrush::Polish) {
            // Gated: see polish_gate. The gate is computed for the whole region
            // first, because it is averaged over the ring — a per-vertex gate
            // switches from 1 to 0 across a single edge and leaves a bead of
            // unsmoothed vertices along every feature it protected.
            out[i] = relax * (w * st * scratch.gate[i]);
        } else {
            // Scrape: the cut and the relax together, both against the snapshot.
            const kernel::cfloat3 cut =
                plane_offset(p, s.plane_point, s.plane_normal, field::FlattenMode::CutOnly);
            out[i] = cut * (w * st) + relax * (w * 0.5f);
        }
    }
}

// RELAX — even the vertex spacing without reshaping the surface.
//
// Smooth moves toward the Laplacian average, which is INWARD on a convex region
// — that is why smoothing shrinks. Relax takes the same target and removes its
// normal component, so a vertex slides across the surface toward the centroid
// of its neighbours and the shape stays.
//
// It is not exactly shape-preserving, and this is the one verb where that is
// worth saying in the code rather than only in the docs: sliding along a
// TANGENT PLANE leaves a curved surface by a second-order amount, so a relax
// pass on a sphere shrinks it slightly. Re-projecting onto the pre-stamp
// surface would fix that and turns a cheap verb into a closest-point query per
// vertex; the drift is measured in examples/56 and is far below what smooth
// moves at the same strength.
void kernel_relax(const SculptSnapshot& s, const SculptNeighbors& nb,
                  const MeshBrushSettings& settings, SculptScratch& scratch,
                  kernel::cfloat3* out) {
    smooth_targets(s, nb, settings.smooth_iterations, &scratch.smoothed, &scratch.smooth_tmp);
    const float st = std::clamp(settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < s.count; ++i)
        out[i] = tangential(scratch.smoothed[i] - s.positions[i], s.normals[i]) *
                 (s.weights[i] * st);
}

// LAYER — deposit to a CEILING rather than accumulating.
//
// Every other deposit verb adds to wherever the surface now is, so a slow
// stroke digs deeper than a fast one over the same path. This one measures
// against where the surface was when the STROKE began and stops at
// `layer_height` above it, so the same path gives the same result at any speed.
void kernel_layer(const SculptSnapshot& s, const MeshBrushSettings& settings,
                  const kernel::cfloat3* origin, kernel::cfloat3* out) {
    const kernel::cfloat3 dir = deposit_direction(s, settings);
    const float ceiling = settings.layer_height;
    for (std::size_t i = 0; i < s.count; ++i) {
        // How far this vertex has already travelled along the deposit
        // direction since the stroke began.
        const float travelled = kernel::cdot(s.positions[i] - origin[i], dir);
        // What is left of this vertex's share of the ceiling. The weight scales
        // the CEILING, not the step, so the falloff shapes the layer's profile
        // and repeated stamps converge on it instead of past it.
        const float remaining = ceiling * s.weights[i] - travelled;
        // Signed: a negative height digs to a floor, and the clamp has to run
        // the other way for it.
        const float step = ceiling >= 0.0f ? std::max(remaining, 0.0f) : std::min(remaining, 0.0f);
        out[i] = dir * (step * std::clamp(settings.strength, 0.0f, 1.0f));
    }
}

// -- the colour pair ----------------------------------------------------------
//
// Neither kernel writes a position. That is the mirror of the property the
// displacement verbs guarantee about colours, and it is what lets a host run a
// colour pass over a finished sculpt without a diff on the geometry.

// PAINT — blend toward the target by the brush's own weight.
//
// `weights` already carries the falloff, the mask gate and the alpha stamp, so
// this composes with all three without a line of code about any of them.
void kernel_paint(const SculptSnapshot& s, const MeshBrushSettings& settings,
                  const kernel::cfloat3* current, kernel::cfloat3* out) {
    const float st = std::clamp(settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < s.count; ++i)
        out[i] = blend_color(current[i], settings.color, s.weights[i] * st);
}

// SMEAR — drag colour across the surface.
//
// For each vertex, blend toward the one-ring neighbour lying most nearly
// OPPOSITE the drag: that is where the colour under the cursor just came from.
// The weight is scaled by how well that neighbour lines up, so a neighbour at
// right angles to the drag contributes nothing and the smear has a direction
// rather than being a smooth.
//
// Neighbours OUTSIDE the region are read too — the colour being dragged in at
// the leading edge has to come from somewhere, and clamping to the region would
// make the stroke's rim smear against itself.
void kernel_smear(const SculptSnapshot& s, const SculptNeighbors& nb,
                  const MeshBrushSettings& settings, const kernel::cfloat3* current,
                  kernel::cfloat3* out) {
    const float st = std::clamp(settings.strength, 0.0f, 1.0f);
    const kernel::cfloat3 drag = settings.direction;
    if (is_zero(drag)) return;  // no direction, no smear — not a smooth
    const kernel::cfloat3 from = safe_normalize(drag, kernel::cf3(0, 0, 0)) * -1.0f;

    for (std::size_t i = 0; i < s.count; ++i) {
        if (s.weights[i] <= 0.0f) continue;
        const std::uint32_t begin = nb.offsets[i], end = nb.offsets[i + 1];
        float best = 0.0f;
        kernel::cfloat3 source = current[i];
        for (std::uint32_t k = begin; k < end; ++k) {
            const kernel::cfloat3 step = nb.positions[k] - s.positions[i];
            const kernel::cfloat3 dir = safe_normalize(step, kernel::cf3(0, 0, 0));
            if (is_zero(dir)) continue;
            const float align = kernel::cdot(dir, from);
            if (align <= best) continue;
            best = align;
            // A neighbour inside the region is read at its PRE-STAMP colour,
            // so the smear is simultaneous rather than a sweep whose result
            // depends on the order the entries happen to sit in.
            const std::uint32_t slot = nb.slots[k];
            source = slot != kOutsideRegion ? current[slot] : nb.colors[k];
        }
        if (best <= 0.0f) continue;  // nothing upwind of this vertex
        out[i] = blend_color(current[i], source, s.weights[i] * st * best);
    }
}

}  // namespace mesh
}  // namespace clay
