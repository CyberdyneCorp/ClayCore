#pragma once

// Brush stroke engine (brush-engine spec): a finger drag becomes a list of
// stamps, and a stamp becomes an ordinary edit.
//
// The interface is deliberately "stroke samples in, edit items out". Resolving
// a stroke is PURE — samples and a preset go in, stamps come out, no document
// is read or touched — and applying the result appends ordinary nodes to a
// layer's edit list, or ordinary brush applications to a voxel grid. Nothing
// here evaluates a stroke through a private path.
//
// That is what makes undo, stroke coalescing, `.clayspace` serialization and
// picking work on stroked edits without any of them knowing this module
// exists: they already work on edits. A brush with its own evaluation path
// would need every one of them reimplemented, and would give the engine a
// second meaning for "an edit" to keep consistent with the first.
//
// It is also where a mask reaches SDF edits. An SDF item is declarative and
// has no per-point strength, so it cannot be gated where it is evaluated
// without destroying the rigidity and finite support that per-brick culling
// depends on. It is gated here instead, where the item is authored: a stamp
// landing in a frozen region is simply not emitted.

#include <cstdint>
#include <optional>
#include <vector>

#include "clay/math/transform.h"
#include "clay/mesh/sculpt.h"
#include "clay/scene/document.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace brush {

// What a stylus reports at one moment of a drag.
struct StrokeSample {
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    float pressure = 1.0f;  // [0,1]
    float tilt = 0.0f;      // radians from the surface normal; 0 is upright
};

// How pressure drives a stamp. Each is an exponent on the normalized
// pressure: 1 is linear, >1 favours light touches, <1 favours firm ones.
// Zero disables the channel, which is what "size only" and "flow only"
// brushes are.
struct PressureResponse {
    float size = 0.0f;      // 0 = pressure does not change the radius
    float strength = 1.0f;  // 0 = pressure does not change the strength
    float curve = 1.0f;     // exponent applied to pressure before either
};

// How overlapping stamps combine.
enum class Accumulation : std::uint8_t {
    // Each stamp applies at its own strength; passing over a spot twice acts
    // twice. The usual sculpting behaviour.
    Buildup = 0,
    // A stroke reaches its strength once and no further, however many stamps
    // overlap: the per-stamp strength is divided by the expected overlap.
    Clamped = 1,
};

// The schema version a preset serializes with. It exists from the first
// release rather than being retrofitted: presets outlive engine versions, and
// a library of them silently reinterpreted by a later build is the failure
// this number is here to prevent.
inline constexpr std::uint16_t kPresetVersion = 1;

struct StrokePreset {
    // Distance between stamps as a fraction of the brush DIAMETER. 0.25 is a
    // dense stroke; 1.0 places stamps just touching. Must be > 0.
    float spacing = 0.25f;

    PressureResponse pressure;

    // Jitter, all as a fraction of the brush radius except rotation, which is
    // in radians. Derived from the stamp index and `seed`, never from a random
    // source, so a stroke resolves identically everywhere.
    float jitter_position = 0.0f;
    float jitter_size = 0.0f;
    float jitter_rotation = 0.0f;
    std::uint32_t seed = 0;

    // Turn each stamp to follow the path. Off by default: it only matters for
    // stamps that are not rotationally symmetric.
    bool rotate_along_stroke = false;

    // Fraction of the stroke's length over which the radius ramps in at the
    // start and out at the end. 0 disables. Their sum is clamped to 1.
    float taper_start = 0.0f;
    float taper_end = 0.0f;

    // Steady stroke ("lazy mouse"): the emission point trails the cursor,
    // smoothing a shaky path. 0 follows exactly, approaching 1 lags more.
    float steady = 0.0f;

    Accumulation accumulation = Accumulation::Buildup;

    // Base radius in world units, and base strength. Pressure, taper and
    // jitter modulate these.
    float radius = 0.25f;
    float strength = 1.0f;

    // Round trip. serialize() writes kPresetVersion; deserialize() accepts
    // that version and any earlier one, taking defaults for whatever the
    // older schema did not carry, and REFUSES a newer one rather than reading
    // a prefix of it and pretending.
    std::vector<std::uint8_t> serialize() const;
    static std::optional<StrokePreset> deserialize(const std::uint8_t* data, std::size_t size);
};

// One resolved stamp: where an edit goes and how strong it is. Everything a
// consumer needs, and nothing about what kind of edit it becomes.
struct Stamp {
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    float radius = 0.0f;
    // The per-application strength an ACCUMULATING consumer reads — what a
    // voxel brush stamps with, and what scales a relief amplitude. Under
    // clamped accumulation it is divided by the expected overlap, so however
    // many stamps cover a point the stroke reaches its strength once.
    float strength = 1.0f;
    // The same strength WITHOUT the clamped-accumulation division: the
    // fraction of the stamp a non-accumulating consumer deposits. Overlapping
    // unions do not add up, so dividing an ADD stamp's deposit by the overlap
    // would shrink every stamp of a clamped stroke rather than keep the stroke
    // at its strength. Equal to `strength` under buildup accumulation.
    float deposit = 1.0f;
    math::Quat rotation = math::Quat::identity();
    float along = 0.0f;  // [0,1] position along the stroke, for a caller's use
};

// The pure core. An empty sample list yields no stamps; a single sample, or a
// path shorter than one spacing, yields exactly one stamp at the start —
// a tap has to leave a mark.
std::vector<Stamp> resolve_stroke(const std::vector<StrokeSample>& samples,
                                  const StrokePreset& preset);

// -- consumers ----------------------------------------------------------------
// Both take the mask the same way the voxel brushes do: a stamp centred in a
// fully masked region is dropped, and a partially masked one has its strength
// scaled by (1 - mask).

// Apply stamps to a voxel grid as ordinary brush stamps. `shape` and `falloff`
// are the footprint; `index` is the palette entry to set, or 0 to erase.
// Returns the number of stamps that actually ran (masked ones do not).
std::size_t apply_to_grid(voxel::VoxelGrid& grid, const std::vector<Stamp>& stamps,
                          std::uint8_t index, voxel::BrushShape shape = voxel::BrushShape::Sphere,
                          voxel::BrushFalloff falloff = voxel::BrushFalloff::Smooth,
                          const voxel::MaskField* mask = nullptr);

// Paint a mask from the same stamps. The third consumer, and the one that makes
// masking a GESTURE rather than a data structure: spacing, pressure, taper,
// steady stroke and jitter reach a mask stroke because they are resolved before
// anything knows what a stamp will become.
//
// `target` is where each cell moves TO — 1 masks and 0 releases — so painting
// and erasing are the same call, as they are on MaskField::paint.
//
// It owns the one conversion a caller would get wrong: a Stamp carries a WORLD
// radius while a mask footprint is sized in MASK CELLS, so a caller doing it by
// hand gets a stroke whose width changes when the mask's resolution does.
//
// No mask parameter: a mask does not gate its own painting.
//
// Accumulation means what it says, against a field whose paint moves toward the
// target rather than adding to it: Buildup approaches the target as stamps
// overlap and cannot pass it, Clamped reaches it once however many overlap.
std::size_t apply_to_mask(voxel::MaskField& mask, const std::vector<Stamp>& stamps, float target,
                          voxel::BrushShape shape = voxel::BrushShape::Sphere,
                          voxel::BrushFalloff falloff = voxel::BrushFalloff::Smooth);

// Turn stamps into edit-list nodes: one node per stamp, `templ` copied and
// re-placed at the stamp's transform, with its radius scaled.
//
// A stamp's strength reaches an item where scaling preserves what its op
// means: relief and incise scale their amplitude (`blend.k`) by it, and an
// Add stamp scales its whole deposit — scale, rounding and blend together —
// so strength 0 deposits nothing and 1 is exactly the item as authored. Every
// other op ignores it; see the comment in stamps_to_nodes for why.
//
// The nodes are returned rather than inserted, so the caller decides which
// commands carry them — one AddNodeCmd per node inside an undo group is what
// makes a whole stroke one undo step, using the stack's existing grouping.
// `content` is the layer's content, used only to reserve node ids: the
// command vocabulary preserves the ids it is given (that is what makes replay
// and undo exact), so a node handed to it must already have one.
std::vector<scene::Node> stamps_to_nodes(scene::SdfContent& content,
                                         const std::vector<Stamp>& stamps,
                                         const scene::Node& templ,
                                         const voxel::MaskField* mask = nullptr);

// -- the fourth consumer: a mesh layer's own triangles -------------------------
//
// `resolve_stroke` was built to be consumed more than once, and this is the
// fourth: spacing, pressure response, deterministic jitter, taper, steady
// stroke and buildup-versus-clamped accumulation reach FIXED-TOPOLOGY MESH
// SCULPTING with no new machinery, exactly as they reached the mask brush. The
// per-stamp strength is what turns one `Clay` stamp into ClayBuildup.
//
// SPACES, because this is the one call in the engine that straddles two. The
// stamps, the settings and the sculptor's mesh are all in the MESH's OWN
// space. The MASK is world-addressed by design (see voxel/mask.h), so
// `mesh_to_world` is how a vertex is found on the mask lattice — identity when
// the layer is untransformed, which is the common case and the default.
//
// Nothing here enters a tape, an edit list or the parity fixture. A sculpted
// mesh layer is still never evaluated.
struct MeshStrokeOptions {
    // Recompute normals once at the end of the stroke rather than per stamp.
    // Faster, identical result; a live preview wants it off.
    bool defer_normals = false;

    // Where the mesh sits, for sampling the mask alone.
    math::Transform mesh_to_world = math::Transform::identity();
};

// Apply a resolved stroke to a mesh. Returns the number of stamps that moved a
// vertex — a stamp that reached nothing, or that landed in a fully masked
// region, is not one of them.
//
// `settings.radius` and `settings.strength` are IGNORED: each stamp brings its
// own, which is what makes pressure and taper shape a mesh stroke exactly as
// they shape a voxel one. Everything else in `settings` is the brush.
//
// GRAB anchors on the FIRST stamp and drags by the motion between stamps;
// SNAKEHOOK re-anchors on every stamp, so its region walks with the pull. That
// one difference is the whole of the difference between them, and it lives
// here rather than in the verb because it is a fact about a STROKE.
//
// `deltas`, when given, accumulates the whole call into one coalesced record —
// one gesture, one undo step.
std::size_t apply_to_mesh(mesh::MeshSculptor& sculptor, const std::vector<Stamp>& stamps,
                          mesh::MeshBrush verb, const mesh::MeshBrushSettings& settings,
                          const voxel::MaskField* mask = nullptr,
                          mesh::VertexDeltas* deltas = nullptr,
                          const MeshStrokeOptions& options = {});

// -- snakehook ----------------------------------------------------------------
//
// Pulling a horn, a tendril or a spike out of a form: the brush that makes a
// sphere into a creature.
//
// NOT a new mechanism, which is worth saying because it was scoped as one. The
// stroke opcode already sweeps a sphere along a segment chain with a radius per
// point, and that IS a tendril when the radii taper — a tapered stroke
// smooth-unioned onto a body reads as a snakehook, and the field stays exact,
// unlike a loft or a sweep. What did not exist is the step that turns a DRAG
// into that item. Same argument as the cut tool: leaving it to each caller
// means each one answers "where does it anchor" and "how does the radius taper"
// differently, and a tendril that detaches from the surface or beads along its
// length is the result.
//
// It ADDS material rather than moving it. ZBrush pulls existing surface, so the
// body dimples slightly where the tendril came from; this grows a tendril and
// leaves the body alone. The difference shows only at the base, and conserving
// volume is a different brush rather than a better snakehook.

struct SnakehookSettings {
    // The tendril's radius where it leaves the surface, in world units.
    float base_radius = 0.2f;

    // Where it ends, as a fraction of the base. Floored below so a tendril
    // finishes in a point that exists rather than one too small for the
    // sampling to carry.
    float tip_fraction = 0.15f;
    float min_tip_radius = 0.01f;

    // Shapes the taper along arc length, as (1 - t) raised to it: 1 is linear,
    // ABOVE 1 thins away quickly and leaves a long thin whip, BELOW 1 holds the
    // thickness and then drops, which is the shape of a horn.
    float taper_curve = 1.0f;

    // Tessellation tolerance for the path, the same document-level quantity a
    // curve uses.
    float tolerance = 0.01f;
};

// Resolve a drag into a tendril. `anchor` is the point on the surface the drag
// started from and `inward` points into the body from there — the caller has
// both from the pick it already did, so no camera and no picking enters here.
// `path` is the drag, in world units.
//
// The anchor is PREPENDED to the path, so the tendril begins where the user
// touched rather than wherever the first drag sample happened to land. Those
// differ: a pick reports the surface, and the first sample arrives a frame
// later with the finger already moving.
//
// It is NOT pushed inward. That was tried, as a fraction of the base radius,
// on the theory that a tendril anchored on the surface would leave a neck. It
// does not: the sweep from a surface point already overlaps the body by its own
// radius, so a deeper anchor only adds material where the body is solid
// anyway. Measured — the field around the base is identical at every depth.
//
// Pure: no document is read or touched, so a caller can preview a tendril
// before committing it. Returns nothing for a drag with no points, a path with
// no length, or a degenerate normal, rather than an item that would sit at the
// origin or contribute nothing.
std::optional<scene::Node> snakehook(kernel::cfloat3 anchor, kernel::cfloat3 inward,
                                     const std::vector<kernel::cfloat3>& path,
                                     const SnakehookSettings& settings = {});

}  // namespace brush
}  // namespace clay
