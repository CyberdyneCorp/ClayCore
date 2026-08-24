#pragma once

// Fixed-topology mesh brushes (meshing spec): the classical sculpting mode, on
// a mesh layer's own triangles.
//
// ONE LINE IS HELD ABOVE EVERYTHING ELSE HERE: **topology never changes.** No
// verb in this file creates, splits, deletes or reorders a polygon or a vertex.
// `Mesh::indices` and `Mesh::quads` are read and never written; a quad mesh
// sculpted here is still the same quad mesh, corner for corner. That is not a
// first-milestone limitation, it is the contract — it is the entire reason
// these verbs are worth having, because the alternative for editing a mesh
// layer is `Volume::from_mesh`, which resamples the model onto a lattice and
// destroys the retopology somebody just paid for.
//
// The consequence is stated rather than hidden: a large grab STRETCHES
// triangles, and `Snakehook` stretches them to the extreme. That is the
// artist's information that the mesh wants retopo, exactly as Blender behaves
// with Dyntopo off. `brush::snakehook` — the SDF resolver — remains the verb
// for GROWING new volume.
//
// This does not change what a document evaluates to. A mesh layer still never
// enters a tape, never blends with a field, and exports exactly as its (now
// edited) vertices say.
//
// WHY THIS DOES NOT REUSE voxel::BrushFalloff. `voxel` depends on `mesh`
// (`VoxelGrid::mesh_greedy` returns a `mesh::Mesh`), so `mesh` including
// `voxel` would be a cycle. `MeshFalloff` below is deliberately the same four
// curves with the same values and the same weights as `voxel::BrushFalloff`,
// and the duplication is the module layering rather than an oversight. The
// MASK is in `voxel` for the same reason and does not appear here at all: a
// verb takes a `field::MaskGate`, and `brush::apply_to_mesh` — which is
// allowed to see both — is the one place a `MaskField` becomes one.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/field/flatten.h"  // FlattenMode
#include "clay/field/relax.h"    // MaskGate
#include "clay/mesh/adjacency.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/lattice.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

// Falloff over the normalized distance from the brush centre. Same curves,
// same values and same weights as `voxel::BrushFalloff`; see the header note.
enum class MeshFalloff : std::uint8_t {
    Constant = 0,
    Linear = 1,
    Smooth = 2,  // smoothstep
    Gaussian = 3,
};

// The classical vocabulary. Each is a distinct vertex operation; the second
// group composes from the first, but each composition is ONE stamp against ONE
// snapshot rather than a sequence of calls — see `BrushRegion`.
enum class MeshBrush : std::uint8_t {
    // -- the primitives ------------------------------------------------------
    Grab = 0,     // drag the region by the stroke delta
    Draw = 1,     // displace along the REGION's averaged normal
    Inflate = 2,  // displace along EACH VERTEX's own normal, signed
    Smooth = 3,   // Laplacian average over the one-ring
    Pinch = 4,    // signed tangential gather (+) / spread (-) about the centre
    Flatten = 5,  // project toward a plane, in one of three modes
    // -- the compositions ----------------------------------------------------
    Clay = 6,        // draw's deposit CLAMPED to a plane at the stamp height
    Crease = 7,      // a tight negative draw and a pinch, in one stamp
    Scrape = 8,      // flatten cut-only and smooth, from one snapshot
    Polish = 9,      // smooth GATED by dihedral angle: noise goes, edges stay
    Snakehook = 10,  // grab re-anchored along the drag
    // -- the three the vocabulary was missing ---------------------------------
    // Slide vertices ALONG the surface to even their spacing, rather than
    // toward the Laplacian average. Smooth reshapes; this redistributes. It
    // matters more here than in a tool that can subdivide: topology is fixed,
    // so a large grab stretches the triangles it has and this is what recovers
    // them without a round trip through a retopo pass.
    Relax = 11,
    // Deposit up to a fixed height above the surface as it was when the STROKE
    // began, and no further. Every other deposit verb accumulates, so a slow
    // stroke digs deeper than a fast one over the same path; this one does not.
    // Needs the stroke's VertexDeltas to know where it started — see `stamp`.
    Layer = 12,
    // Push material along the surface in the drag direction. Grab carries the
    // region rigidly; this slides it.
    Nudge = 13,
    // -- the colour pair, and the only verbs that do not move a vertex --------
    // Blend each vertex's colour toward `MeshBrushSettings::color` by the
    // brush's own per-vertex weight, so falloff, strength, the geodesic walk,
    // the mask gate and the alpha stamp all compose with it for free.
    Paint = 14,
    // Push existing colour along `direction`, by blending each vertex toward
    // the one-ring neighbour lying most nearly OPPOSITE the drag. Topology is
    // fixed here, so the one-ring is a complete and cheap account of where the
    // colour just came from — no spatial query, and no interpolation scheme to
    // disagree with the rest of the library about.
    Smear = 15,
};

// Whether a verb writes `Mesh::colors` instead of moving vertices. The two are
// exclusive on purpose: a colour pass over a finished sculpt must not show up
// as a diff on the geometry, and a displacement verb must not disturb an
// imported model's colours.
bool writes_color(MeshBrush verb);

inline constexpr std::uint32_t kNoClass = 0xffffffffu;

// The most smoothing passes one stamp will run. A bound rather than a
// preference: each pass walks the whole region again, so a count arriving from
// a host's slider typo is otherwise an unbounded amount of work.
inline constexpr int kMaxSmoothIterations = 64;

struct MeshBrushSettings {
    // Where the stamp lands, in the mesh's own space, and how far it reaches.
    kernel::cfloat3 center = kernel::cf3(0, 0, 0);
    float radius = 0.25f;

    // Signed for every verb that has a sign: Inflate and Draw deposit or dig,
    // Pinch gathers (+) or spreads (-), Crease cuts (+) or raises a ridge (-).
    // Scaled into world units by `radius`, so a brush behaves the same at any
    // size and a strength of 1 moves the surface by about one radius.
    float strength = 0.5f;

    MeshFalloff falloff = MeshFalloff::Smooth;

    // Grab and Snakehook only: the motion THIS stamp applies, in world units.
    // The other verbs ignore it.
    kernel::cfloat3 direction = kernel::cf3(0, 0, 0);

    // Draw, Clay and Crease: an explicit deposit direction. Zero — the default
    // — means "the region's averaged normal", which is what makes Draw a
    // rounded organic swell instead of a balloon.
    kernel::cfloat3 deposit_normal = kernel::cf3(0, 0, 0);

    // Measure the falloff ALONG THE SURFACE rather than in a straight line.
    // The Move Topological rule: a brush on the upper lip must not drag the
    // chin through the closed mouth. Off for the verbs whose meaning is
    // "everything under this disc" — see `default_geodesic`.
    bool geodesic = true;

    // The geodesic walk's seed, when the caller knows it — any class of the
    // triangle the pick hit. Without one the seed is found by a linear scan
    // over the classes, which is fine for a test and wrong for a stroke on a
    // million-vertex mesh.
    std::uint32_t seed_class = kNoClass;

    // Flatten and Scrape. CutOnly is the hard-surface family — Trim Dynamic,
    // hPolish, the Planar brushes — where cutting WITHOUT filling is the whole
    // brush.
    field::FlattenMode flatten_mode = field::FlattenMode::TwoSided;

    // An explicit plane for Flatten and Scrape. Without one the plane is the
    // region's weighted centroid with the region's weighted average normal —
    // the plane ZBrush and Blender both use, and the one an artist means by
    // "flatten what is under the brush".
    bool use_given_plane = false;
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    // Polish: how far the surface around a vertex may bend before the
    // smoothing fades out, in radians. Full strength up to this angle, zero at
    // twice it — so an edge survives a pass that removes the noise beside it.
    //
    // The default is deliberately TIGHT. Measured on a dented box (see
    // examples/46), 0.2 removes marginally more roughness than a plain smooth
    // while moving the box's edges a third as far; by 0.45 the gate is open
    // almost everywhere and polish is a plain smooth wearing a different name,
    // which is not a useful default for a brush called polish.
    float polish_angle = 0.20f;

    // Smooth, Polish, Scrape and Relax: passes per stamp. Bounded by
    // kMaxSmoothIterations.
    int smooth_iterations = 1;

    // Layer only: how far above the stroke's STARTING surface the deposit may
    // reach, in WORLD units.
    //
    // World rather than radius-relative, unlike `strength`, and that is the
    // point rather than an inconsistency: a ceiling that moved when the brush
    // resized would not be a ceiling. Negative digs to a floor instead.
    float layer_height = 0.05f;

    // -- alpha ---------------------------------------------------------------
    // A caller-supplied scalar stamp scaling this brush's per-vertex weight, so
    // detail work on a mesh layer is alpha-driven as it already is on voxels
    // (sculpt_carve_alpha) and on SDF items (Deformer::alpha).
    //
    // THE ENGINE DECODES NO IMAGES: `alpha` is `alpha_width * alpha_height`
    // samples in [0,1], row-major with u fastest, BORROWED for the duration of
    // the call. Null — the default — leaves every verb exactly as it was.
    //
    // Sampled by the kernel's `calpha_sample`, the same function the SDF alpha
    // uses, so one stamp reads identically on a mesh and on a field rather than
    // through two bilinear lookups that could drift apart.
    //
    // It multiplies the WEIGHT, so it composes with every verb and every
    // falloff without a line of per-verb code.
    const float* alpha = nullptr;
    int alpha_width = 0;
    int alpha_height = 0;
    // The square the stamp covers, in the plane through `center` whose normal
    // is `alpha_direction`; `alpha_tangent` orients it there and any rough "up"
    // works, since it is re-orthogonalised. Zero extent means the brush's own
    // diameter, which is what a host stamping under the cursor wants.
    kernel::cfloat3 alpha_direction = kernel::cf3(0, 0, 0);  // 0 = region normal
    kernel::cfloat3 alpha_tangent = kernel::cf3(0, 0, 0);    // 0 = derived
    float alpha_extent = 0.0f;                               // 0 = 2 * radius

    // -- colour ---------------------------------------------------------------
    // Paint's target. Whatever space the caller keeps `Mesh::colors` in: this
    // is blended toward componentwise and never converted, so a linear buffer
    // stays linear and an sRGB one stays sRGB.
    //
    // Smear ignores it — its colour comes from the surface it is dragging
    // across, which is the whole difference between the two verbs.
    kernel::cfloat3 color = kernel::cf3(1, 1, 1);

    bool has_alpha() const { return alpha != nullptr && alpha_width >= 2 && alpha_height >= 2; }
};

// Whether a verb measures its falloff along the surface by default. Flatten
// and Scrape do not: they mean "everything under this disc", and a surface
// walk would refuse to flatten across a groove — which is the one place a
// flatten is most wanted.
bool default_geodesic(MeshBrush verb);

// The PRE-STAMP SNAPSHOT. Everything a verb reads, captured before anything is
// written, which is what lets a composed verb be one operation:
//
//   - Draw takes ONE direction for the whole stamp from `average_normal`, so a
//     stroke does not chase its own deposit into a balloon.
//   - Smooth's Laplacian reads neighbours from here where they are in the
//     region, so it is a simultaneous average rather than a Gauss-Seidel sweep
//     whose result depends on vertex order.
//   - Scrape is flatten-cut-only AND smooth against these same positions, and
//     Crease is a draw AND a pinch against them. Calling the halves in
//     sequence is a different operation and a worse one — the same rule
//     `VoxelGrid::sculpt_scrape` already states.
struct BrushRegion {
    std::vector<std::uint32_t> classes;      // weld classes the falloff reached
    std::vector<float> weights;              // falloff * (1 - mask), in [0,1]
    std::vector<kernel::cfloat3> positions;  // pre-stamp, one per class
    std::vector<kernel::cfloat3> normals;    // pre-stamp, area-weighted, unit

    kernel::cfloat3 average_normal = kernel::cf3(0, 1, 0);
    kernel::cfloat3 centroid = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    bool empty() const { return classes.empty(); }
    std::size_t size() const { return classes.size(); }

    // class -> index into `classes`, kNoClass outside the region. Sized to the
    // adjacency's class count and reset over `classes` alone, so a stamp costs
    // what it reached rather than what the mesh holds.
    std::vector<std::uint32_t> slot;
};

// A sparse, coalesced record of what a gesture moved: the undo a mesh stroke
// cannot get from the edit list, because vertex displacement is destructive
// and is not an edit item.
//
// COALESCED PER GESTURE: a vertex touched by forty stamps of one stroke appears
// once, keeping the FIRST `before` and the LAST `after`. The record's size is
// therefore bounded by the vertices the stroke REACHED, not by the stamps it
// took, and reverting one is one undo step.
//
// Normals are STORED rather than recomputed on revert. An imported mesh's
// normals are whatever its author wrote; recomputing them would restore a mesh
// that is geometrically identical and byte different, and bit-exactness is the
// bar. `indices` and `quads` are not recorded because nothing can change them
// — the contract paying off.
class VertexDeltas {
   public:
    std::size_t size() const { return vertices_.size(); }
    bool empty() const { return vertices_.empty(); }
    const std::vector<std::uint32_t>& vertices() const { return vertices_; }
    void clear();

    // Restore / re-apply. Both are idempotent, and neither touches `indices`
    // or `quads`. Refused (returns false, changing nothing) against a mesh of a
    // different vertex count — that is a caller pairing a record with the wrong
    // mesh, which is worth a refusal rather than a corrupted buffer.
    bool revert(Mesh& m) const;
    bool apply(Mesh& m) const;

    // Where `v` was when this record started following it, or nullopt if it
    // has not been touched yet. Exists for `MeshBrush::Layer`, whose ceiling is
    // measured from the surface as the STROKE found it — the record is already
    // keeping exactly that, so the verb needs no per-stroke state of its own.
    std::optional<kernel::cfloat3> origin_of(std::uint32_t v) const;

    // Capture `v`'s current position, normal and colour, the FIRST time it is
    // seen. Called by the verbs before they write; public because
    // `brush::apply_to_mesh` drives the same record across a whole stroke.
    //
    // Colour is recorded exactly the way normals are — stored rather than
    // recomputed, so an imported model's colours come back byte for byte —
    // and is present only when the mesh carried a colour attribute when the
    // record started following it.
    void note(std::uint32_t v, const Mesh& m);
    // Rewrite `v`'s "after" from the mesh as it now is. Called after a stamp
    // and after a deferred normal recomputation, so the last word wins.
    void sync_after(std::uint32_t v, const Mesh& m);

    // -- encoding (survive-a-crash) -------------------------------------------
    //
    // A mesh step is the only one of the three the session history records that
    // had no byte form: an edit-list step is a scene::Command, which the
    // document format already encodes, and a voxel step is a run of PODs. This
    // is the third, and without it a journal would carry two kinds out of three
    // and say nothing about the missing one.
    //
    // A MEMBER rather than a free function in `session`, because the record's
    // "after" values and its normal/colour flags have no public accessors — and
    // widening the class's read surface just to serialize it from outside would
    // be a worse trade than owning the encoding here, which is also where
    // VoxelGrid keeps its own stream methods.
    //
    // The `slot_` index is NOT encoded: it is derivable from `vertices_`, and
    // storing a hash map's contents would be storing a rebuildable thing.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated or inconsistent buffer rather than returning a record
    // that reverts a mesh to garbage. Returns false and leaves `out` untouched.
    static bool decode(const std::uint8_t* data, std::size_t size, VertexDeltas* out);

   private:
    std::vector<std::uint32_t> vertices_;
    std::vector<kernel::cfloat3> before_position_, after_position_;
    std::vector<kernel::cfloat3> before_normal_, after_normal_;
    std::vector<kernel::cfloat3> before_color_, after_color_;
    std::unordered_map<std::uint32_t, std::uint32_t> slot_;
    bool normals_ = false;
    bool colors_ = false;
};

// A sculpting session over one mesh: the adjacency, the ray-query tree and the
// per-stamp scratch, all of which are expensive to build and cheap to keep.
//
// Held BY REFERENCE. The mesh must outlive the sculptor, and nothing else may
// change its vertex or index count while one exists — which for this feature
// means nothing else may change it at all, since no verb here can.
class MeshSculptor {
   public:
    explicit MeshSculptor(Mesh& m, float weld_epsilon = kDefaultWeldEpsilon);
    // For a caller that already built an adjacency (an importer, a test). The
    // adjacency must match `m`; it is checked.
    MeshSculptor(Mesh& m, Adjacency adjacency);

    const Mesh& mesh() const { return mesh_; }
    Mesh& mesh() { return mesh_; }
    const Adjacency& adjacency() const { return adjacency_; }
    bool valid() const { return adjacency_.matches(mesh_); }

    // Apply ONE stamp. Returns the number of weld classes that moved, which is
    // 0 for a stamp that reached nothing, that was fully masked, or whose
    // settings amount to no displacement.
    //
    // `gate` is the freeze, taken exactly as the field verbs take one: the
    // weight at a vertex is scaled by (1 - gate) at that vertex's world
    // position, so a fully masked vertex is untouched by EVERY verb rather than
    // by a hand-picked few. Empty means no mask and costs nothing.
    //
    // `record`, when given, accumulates into the caller's gesture.
    std::size_t stamp(MeshBrush verb, const MeshBrushSettings& settings,
                      const field::MaskGate& gate = {}, VertexDeltas* record = nullptr);

    // A LATTICE over the whole mesh — ZBrush's Gizmo Lattice, Blender's
    // Lattice modifier. Not a brush: it takes no centre, no radius and no
    // falloff, because a cage IS the falloff. Every vertex moves by the cage's
    // displacement at its own position, which for an untouched cage is exactly
    // zero everywhere.
    //
    // Forward, with no inversion anywhere — see `mesh/lattice.h` for why that
    // is available here and not on an SDF item.
    //
    // Returns how many vertices actually moved, and records into `record` the
    // same way a stamp does, so a lattice is one undo step.
    std::size_t apply_lattice(const Lattice& cage, VertexDeltas* record = nullptr);

    // A whole-form deformer — taper or twist — over every vertex, scaled by
    // the gate. Returns how many vertices moved.
    //
    // The WHOLE mesh rather than a brush region, because a deformer states
    // something about the form and a brush states something about a dab; that
    // is also what ZBrush's Deformation palette does. The gate is what holds
    // part of the form still, and a fully gated vertex is bit-identical to
    // where it started.
    //
    // An identity deformer walks nothing and records nothing.
    std::size_t apply_deformer(const MeshDeformSettings& settings,
                               const field::MaskGate& gate = {},
                               VertexDeltas* record = nullptr);

    // Normals follow the vertices. A moved vertex with a stale normal shades
    // wrong immediately, so this runs per stamp by default — but a host
    // draining a stroke can defer it, which is the choice `defer_normals`
    // gives. Deferring changes nothing about the final mesh.
    //
    // A mesh carrying NO normals still carries none afterwards: they are
    // optional on `mesh::Mesh` and manufacturing them would change what the
    // layer exports.
    // -- the colour attribute -------------------------------------------------
    // Paint and Smear REFUSE a mesh with no colours rather than creating one:
    // allocating twelve bytes per vertex on the first dab hides a real cost
    // behind a brush stroke, and makes "I painted and nothing happened"
    // indistinguishable from "this mesh had no colour attribute". Creating it
    // is therefore something a host does on purpose.
    bool has_colors() const;
    // Give every vertex `fill` if the mesh has no colour attribute. Returns
    // whether it created one; a mesh that already has colours is left exactly
    // as it is, so this is safe to call before every stroke.
    bool ensure_colors(kernel::cfloat3 fill = kernel::cf3(1, 1, 1));

    void set_defer_normals(bool defer) { defer_normals_ = defer; }
    bool defer_normals() const { return defer_normals_; }
    // Recompute the deferred region and clear it. A no-op when nothing is
    // pending. `record` is updated so a deferred stroke's undo is still exact.
    void flush_normals(VertexDeltas* record = nullptr);

    // Where a weld class sits, and which one is nearest a point.
    //
    // The second goes through the ray tree when the sculptor has one, which is
    // O(log N); it falls back to a linear scan over every class when it does
    // not. It used to be the scan always, which is why
    // `MeshBrushSettings::seed_class` exists — a caller could hand the walk its
    // own anchor and skip it. That is no longer necessary: the walk seeds
    // itself from this. Passing `seed_class` is still faster by one query and
    // still the right thing when the host already knows where the finger is.
    kernel::cfloat3 class_position(std::uint32_t cls) const;
    std::uint32_t nearest_class(kernel::cfloat3 p);

    // -- picking -------------------------------------------------------------
    // Built lazily on the first query. Positions move under it, and what a
    // stale tree reports is worth stating precisely, because the obvious guess
    // is wrong: it does NOT report the surface as it was when the tree was
    // built. The hit follows the moved triangle, but it is found through stale
    // bounds, so it drifts OFF the ray — measured by
    // `tests/unit/test_bvh_refit.cpp` at 4.4e-2 from the ray before an update
    // and 1.5e-8 after (reference/host_loop.py reports 6.9e-4 and 3.1e-9 for its
    // own smaller stamp — the ratio is the point, not the absolute). Invisible to a brush, which only wants a
    // depth; the entire error budget of a gizmo, which wants a point.
    //
    // `refit_bvh` is the per-stamp call. It updates the bounds of the triangles
    // the last stamp's region touched and of their ancestors, which is
    // proportional to the brush. `refresh_bvh` rebuilds, which is proportional
    // to the MESH — 1.3 s on a 2M-vertex model against 0.25 ms for the stamp
    // that dirtied it — and is the right call after something moved the mesh
    // BEHIND the sculptor, which a refit cannot know about: a
    // `VertexDeltas::revert` or `::apply` for an undo, or a caller writing
    // positions directly.
    //
    // It is NOT the answer to a rising `bvh().quality()`, however natural that
    // reading is. Measured over five deformations, a rebuild produced a better
    // tree in exactly one of them and was dramatically worse in two — see
    // `Bvh::quality`. Nothing here rebuilds on its own behalf.
    const Bvh& bvh();
    // Whether a tree exists, so a diagnostic can ask what queries cost without
    // BUILDING one behind the caller — `bvh()` is lazy and a first call on a 2M
    // vertex mesh costs 1.3 s.
    bool has_bvh() const { return bvh_ != nullptr; }
    void refresh_bvh();
    // Refits everything moved SINCE THE LAST refit or rebuild, not since the
    // last stamp. That distinction is the whole correctness of the call: a
    // stroke is many stamps, `apply_stroke` consumes them internally, and a
    // set covering only the final dab would leave every earlier dab's
    // ancestors holding pre-stroke bounds — the subset failure `Bvh::refit`
    // forbids, reached through the API this one is meant to pair with. So the
    // sculptor accumulates what it touched and this drains it.
    //
    // A whole-mesh operation (`apply_lattice`, `apply_deformer`) marks
    // everything instead, and a refit after one of those refits the whole tree.
    //
    // A no-op when no tree has been built yet: there is nothing to refit, and
    // the next `bvh()` builds one that is already correct.
    //
    // WHAT IT CANNOT SEE: edits made to the mesh BEHIND the sculptor —
    // `VertexDeltas::revert` and `::apply` take the `Mesh&` directly, so
    // nothing tells the sculptor those vertices moved. Call `refresh_bvh` after
    // an undo or a redo.
    void refit_bvh();

   private:
    void gather(const MeshBrushSettings& settings, const field::MaskGate& gate);
    std::size_t write(VertexDeltas* record);
    void gather_stroke_origin(const VertexDeltas& record);
    // The colour counterpart of `write`: applies `color_target_` where it
    // differs from what the mesh holds, and returns how many classes changed.
    std::size_t write_colors(VertexDeltas* record);
    void recompute_normals(const std::vector<std::uint32_t>& classes, VertexDeltas* record);
    // The ray tree, refitted — or null when the host has never built one, in
    // which case every caller below falls back to the scan it replaced.
    const Bvh* surface_index();
    bool classes_in_ball(kernel::cfloat3 centre, float radius, std::vector<std::uint32_t>* out);
    void mark_bvh_dirty(std::uint32_t cls);
    void clear_bvh_dirty();

    // Layer's per-entry stroke origin, kept as a member so a stroke does not
    // reallocate it per stamp.
    std::vector<kernel::cfloat3> origin_;

    Mesh& mesh_;
    Adjacency adjacency_;
    BrushRegion region_;
    WalkScratch walk_;
    std::vector<float> distance_;
    std::vector<kernel::cfloat3> displacement_;
    // Where each class's colour should END UP, seeded from what it holds now,
    // so a verb that leaves an entry alone writes nothing rather than
    // rewriting a colour with itself.
    std::vector<kernel::cfloat3> color_target_;
    std::vector<kernel::cfloat3> smoothed_, smooth_tmp_;
    std::vector<float> gate_, gate_tmp_;
    std::vector<std::uint32_t> pending_normals_, deferred_normals_;
    std::vector<char> normal_mark_;
    // The triangles handed to `Bvh::refit`, kept as a member so a per-stamp
    // refit does not allocate. Duplicates are harmless: the second sighting of
    // a triangle finds its leaf already marked and walks no further.
    std::vector<std::uint32_t> refit_tris_;
    // Scratch for the ball query, kept so a stamp does not allocate. `ball_mark_`
    // is retired through the result list, never cleared wholesale.
    std::vector<std::uint32_t> ball_tris_;
    std::vector<char> ball_mark_;
    // Classes moved since the last refit or rebuild, as a compact list plus a
    // membership mark. The mark is reset through the LIST rather than cleared,
    // so the cost is what a stroke touched and not what the mesh holds — the
    // same discipline `WalkScratch` uses in adjacency.h.
    std::vector<std::uint32_t> dirty_classes_;
    std::vector<char> class_dirty_;
    // Set by the whole-mesh operations, which have no small dirty set to name.
    bool bvh_all_dirty_ = false;
    std::unique_ptr<Bvh> bvh_;
    bool defer_normals_ = false;
};

}  // namespace mesh
}  // namespace clay
