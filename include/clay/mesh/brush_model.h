#pragma once

// The brush model: the axes a brush composes from (brush-engine spec,
// add-shared-brush-kernels).
//
// WHAT THIS IS FOR. The artist-facing brush families other engines ship —
// ClayBuildup, DamStandard, hPolish, Trim Dynamic, Snake Hook, Rake — are not
// new deformations. Each is a kernel plus a falloff plus a frame plus an
// accumulation rule plus a spacing. Naming those axes separately is what turns
// a brush into a PRESET instead of an engine path, and it is what lets the next
// named brush cost a serialized struct rather than a code path. A new brush
// that needs a new code path is evidence an axis is missing, and the axis is
// what should be added.
//
// ENUMS AND PLAIN DATA, never polymorphic objects. A per-vertex loop that
// dispatches virtually cannot be specialized, serialized or mirrored across a C
// ABI, and this model is required to do all three.
//
// WHY THIS LIVES IN `mesh` AND NOT IN `brush`. `tools/check_layering.py`
// records `brush -> mesh`, because `brush::apply_to_mesh` is the stroke
// engine's fourth consumer. So `mesh` may not include `brush` — and the
// per-vertex loop that has to read these axes is `MeshSculptor::stamp`, here in
// `mesh`. Putting the model in `brush/` would be a cycle on the first include.
// `brush::BrushPreset` — the axes PAIRED WITH a `StrokePreset` — does live in
// `brush`, which is the one module that can see both vocabularies.

#include <cstdint>

#include "clay/mesh/sculpt_common.h"

namespace clay {
namespace mesh {

// How the region under the brush is REACHED. Not how it is weighed: the walk
// decides what is reached and the straight line decides how much, which is a
// distinction `MeshSculptor::gather` records at length.
enum class BrushFootprint : std::uint8_t {
    // Everything within the radius in a straight line. What a verb whose
    // meaning is "everything under this disc" wants — flatten and scrape, which
    // must not refuse to flatten across a groove.
    Ball = 0,
    // Everything reachable ALONG THE SURFACE within the radius. The Move
    // Topological rule: a brush on the upper lip must not drag the chin through
    // the closed mouth.
    SurfaceWalk = 1,
};

// The direction a kernel displaces along, named rather than implied.
//
// THIS AXIS IS WHY DRAW AND INFLATE ARE ONE KERNEL. They were two verbs whose
// documented difference was the direction each takes; naming the direction
// makes them one deformation under two frames, and the results are unchanged
// because that was always the only difference.
enum class BrushFrame : std::uint8_t {
    // The kernel does not displace along a direction at all — the Laplacian
    // family and the colour pair.
    None = 0,
    // The region's averaged normal, ONE for the whole stamp. What makes draw a
    // rounded organic swell rather than a balloon.
    RegionNormal = 1,
    // Each vertex's own normal. What makes inflate inflate.
    VertexNormal = 2,
    // The motion this stamp applies — grab, snakehook and nudge.
    StrokeDirection = 3,
    // The plane a flatten-family verb was given or computed.
    RegionPlane = 4,
};

// The deformation itself. A kernel is a shape of arithmetic rather than a
// verb: `Displace` serves draw and inflate under two frames, and `Laplacian`
// serves smooth, polish, scrape and relax under different readings of one
// target.
enum class BrushKernelId : std::uint8_t {
    Translate = 0,       // carry the region rigidly
    Displace = 1,        // move along the frame
    Gather = 2,          // tangential pull toward or away from the centre
    Tangential = 3,      // slide along the surface in a direction
    Plane = 4,           // project toward a plane, clamped by the mode
    PlaneDeposit = 5,    // deposit up to a plane floating at the stamp height
    CutAndGather = 6,    // one stamp that cuts and squeezes — crease
    Laplacian = 7,       // the one-ring average, read four ways
    DepositCeiling = 8,  // deposit to a ceiling measured from the stroke's start
    ColorBlend = 9,      // paint
    ColorAdvect = 10,    // smear
};

// Which buffer a kernel writes. The two are exclusive on purpose: a colour pass
// over a finished sculpt must not show up as a diff on the geometry, and a
// displacement verb must not disturb an imported model's colours.
enum class BrushWriteTarget : std::uint8_t {
    Position = 0,
    Color = 1,
};

// What has to happen after a stamp writes. A colour pass changes nothing about
// the surface, which is why it owes nothing here.
enum class BrushPostPolicy : std::uint8_t {
    None = 0,
    RecomputeNormals = 1,
};

// One brush, as axis values. Every verb in the vocabulary is a value of this
// type — see `model_of` — and so is every named artist family, which is what
// makes a family a preset rather than a code path.
struct BrushModel {
    MeshBrush verb = MeshBrush::Draw;
    BrushFootprint footprint = BrushFootprint::SurfaceWalk;
    MeshFalloff falloff = MeshFalloff::Smooth;
    BrushFrame frame = BrushFrame::RegionNormal;
    BrushKernelId kernel = BrushKernelId::Displace;
    BrushWriteTarget target = BrushWriteTarget::Position;
    BrushPostPolicy post = BrushPostPolicy::RecomputeNormals;
};

// The axis decomposition of each verb in the vocabulary. This is the table that
// has to stay true: if a verb cannot be written as a row here, an axis is
// missing.
BrushModel model_of(MeshBrush verb);

// -- the compiled plan --------------------------------------------------------

// What one stroke's brush needs, worked out ONCE at the start of the stroke so
// that a stamp reads a flat record instead of re-inspecting a fourteen-field
// settings struct per vertex.
//
// WHAT IS DELIBERATELY NOT HERE: precomputed reciprocals. The obvious one is
// 1/radius, so the falloff's `distance / radius` becomes a multiply — and that
// is not the same number. Division and multiplication by a reciprocal round
// differently, so precomputing it would move the last bit of every weight in
// the library. Bit-parity with the fixed path is this change's acceptance
// criterion and it outranks one divide per vertex.
struct BrushRuntimePlan {
    BrushModel model;

    // What the gather must produce. The neighbour normals are the expensive
    // entry and polish is the only verb that reads them: a geometric normal per
    // neighbour is a walk over the region's whole two-ring, and no other verb
    // would use it.
    bool needs_neighbors = false;
    bool needs_neighbor_normals = false;
    bool needs_neighbor_colors = false;
    // Layer measures its ceiling from where the surface was when the STROKE
    // began, so it needs the gesture's record and refuses without one.
    bool needs_stroke_origin = false;

    // Clamped once here rather than per pass, which is where the bound belongs:
    // a count arriving from a host's slider typo is otherwise an unbounded
    // amount of work per stamp.
    int smooth_passes = 1;

    // Whether the footprint is measured along the surface. Resolved from the
    // model and the caller's override together, so the loop asks one question.
    bool geodesic = true;
};

// Compile a plan. Cheap and pure; the point of caching it is that a stamp does
// not re-derive it, not that deriving it is slow.
BrushRuntimePlan compile_plan(const BrushModel& model, const MeshBrushSettings& settings);

}  // namespace mesh
}  // namespace clay
