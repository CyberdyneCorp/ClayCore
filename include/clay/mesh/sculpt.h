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
// The falloff curves and why they are not `voxel::BrushFalloff` are in
// `sculpt_common.h`, with the vocabulary they belong to. The MASK is in
// `voxel` for the same layering reason and does not appear here at all: a verb
// takes a `field::MaskGate`, and `brush::apply_to_mesh` — which is allowed to
// see both — is the one place a `MaskField` becomes one.
//
// THE DEFORMATION MATH IS NOT HERE EITHER. It is in `sculpt_kernels.h`, behind
// an interface that names no mesh and no vertex, because the adaptive and
// multiresolution sculptors call the same kernels. What stays in this file is
// the fixed-topology plumbing that feeds them: the weld-class gather, the
// write-back, the local normal recompute and the BVH bookkeeping.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/field/flatten.h"  // FlattenMode
#include "clay/field/relax.h"    // MaskGate
#include "clay/mesh/adjacency.h"
#include "clay/mesh/brush_arena.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/lattice.h"
#include "clay/memory/budget.h"  // PeakTelemetry
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/brush_model.h"
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/sculpt_kernels.h"
#include "clay/mesh/sculpt_workset.h"

namespace clay {
namespace mesh {

// The verb vocabulary, the falloff curves and `MeshBrushSettings` live in
// `sculpt_common.h` so that a sculptor over a different representation can name
// a brush without including this file's fixed-topology machinery. They are
// re-exported here, so every existing caller of `mesh/sculpt.h` sees exactly
// what it saw before.

// The workset — the pre-stamp snapshot of everything under the brush — is in
// `sculpt_workset.h`, because the adaptive and multiresolution sculptors gather
// one too and must not each invent their own.

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
    // What this record OWNS, for a memory budget. Not sizeof: the arrays are
    // the payload, and a record following one vertex costs nothing like one
    // following a stroke.
    std::size_t bytes() const;

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

    // How many times a plan has been compiled over this sculptor's life. The
    // brush-engine requirement is that a stroke compiles its preset ONCE and
    // every stamp reads the compiled plan, and a count is the only way a test
    // can tell the difference between that and recompiling silently.
    std::size_t plan_compilations() const { return plan_compilations_; }

    // -- what the last stamp touched -----------------------------------------
    //
    // The WRITE REGION: the weld classes the last stamp actually moved (or
    // recoloured), and the bounds of that motion. Not the workset — the rim of
    // a falloff and a fully masked vertex are gathered and never move — and not
    // the read halo either, which is the ring Smooth, Relax and Polish average
    // over without touching. A host uploading the halo would re-send geometry
    // that did not change.
    // WIDENED TO `WorkItemId` (add-shared-brush-runtime): the write region is
    // the workset's own array, and the workset stopped being addressed in weld
    // classes when the adaptive and multiresolution sculptors started filling
    // one. Carrying a second, narrower array holding the same information would
    // have been a second answer to "what did this stamp write" — the exact
    // failure `sculpt_workset.h` exists to prevent. Call `as_weld_class()` on
    // an entry to get the number this used to hand back.
    const std::vector<WorkItemId>& write_region() const { return region_.write_region; }
    const math::Aabb& write_bounds() const { return region_.write_bounds; }
    // The whole workset, including the entries that did not move. For a caller
    // that wants to know what the brush REACHED rather than what it changed.
    const SculptWorkset& workset() const { return region_; }

    // The automask factors `mesh` may not compute for itself — the cavity
    // estimator and the surface-group field, both of which live in modules that
    // depend on this one. Set once for a STROKE: they hold `std::function`s,
    // and copying those per stamp would allocate on every dab.
    void set_automask_inputs(AutomaskInputs inputs) { automask_inputs_ = std::move(inputs); }
    const AutomaskInputs& automask_inputs() const { return automask_inputs_; }

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

    // -- the seed token ------------------------------------------------------
    // What a caller stores beside a `seed_class` it picked, and sends back in
    // `MeshBrushSettings::seed_revision` so a stamp can tell a live seed from
    // one taken out of a numbering that no longer exists.
    //
    // It identifies the CLASS SPACE, not the positions: this sculptor's
    // adjacency is fixed for its whole life (both constructors take one and
    // nothing rebuilds it), so vertices moving under a stroke leave the token
    // alone — a seed stays valid across the stamps of a stroke, which is
    // exactly when re-picking would be wasted. What DOES retire a token is a
    // new sculptor, and a hierarchy makes one on every rebind, which is the
    // case `seed_revision` was added for.
    //
    // A monotonic counter rather than a hash of the adjacency: a hash costs a
    // walk over the whole class space to compute and would make two sculptors
    // over identical topology interchangeable, which they are not — they index
    // different `Mesh` storage.
    std::uint64_t seed_revision() const { return seed_revision_; }
    // How many stamps rejected a seed because its revision did not match. The
    // only way a test can prove the rejection HAPPENED rather than the seed
    // having been harmless, which is the difference between this gate and one
    // that passes because the walk found its way anyway.
    std::size_t stale_seeds_rejected() const { return stale_seeds_rejected_; }

    // -- peak telemetry (task 7.7) -------------------------------------------
    // Where this sculptor publishes the high-water mark of its WORKSET, for a
    // host tuning a `SculptMemoryProfile`. Borrowed and never owned; null is
    // the default and the only cost is a null check once per stamp.
    //
    // The peak rather than the last value, and the workset rather than the
    // write region: what a stamp has to hold is everything it GATHERED,
    // including the rim of the falloff that never moves, and a host sizing an
    // arena against the write region would size it against the wrong number.
    void set_telemetry(memory::PeakTelemetry* telemetry) { telemetry_ = telemetry; }
    memory::PeakTelemetry* telemetry() const { return telemetry_; }

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

    // What the per-stamp scratch arena owns and how far it has had to grow.
    //
    // ONE PER SCULPTOR AND NEVER A PROCESS-GLOBAL: a `MultiresSculptor` owns a
    // `MeshSculptor`, and a document can hold several mesh layers, so a shared
    // arena would make two stamps alias each other's scratch and would be a
    // data race the first time a host stamped two layers on two threads.
    const BrushScratchArena& arena() const { return arena_; }

   private:
    // The walk: everything the brush REACHES, into `candidates_` and
    // `distance_`. Declared here rather than in `sculpt_workset.h` because it
    // names an `Adjacency` and a `Bvh`, and a neutral header holding three
    // representation-specific signatures is neutral in the directory listing
    // only.
    void build_fixed_mesh_workset(const MeshBrushSettings& settings);
    void gather(const MeshBrushSettings& settings, const field::MaskGate& gate);
    static kernel::cfloat3 normal_of_item(const void* context, WorkItemId item);
    std::size_t write(VertexDeltas* record);
    void gather_stroke_origin(const VertexDeltas& record);
    // The colour counterpart of `write`: applies `color_target_` where it
    // differs from what the mesh holds, and returns how many classes changed.
    std::size_t write_colors(VertexDeltas* record);
    void recompute_normals(const std::vector<std::uint32_t>& classes, VertexDeltas* record);
    // The region and its one-rings, in the shape the shared kernels read. No
    // copy: the region already holds its arrays contiguously and in parallel,
    // which is what makes handing them over free.
    // The colour half of a stamp. Lifted out of `stamp` so the displacement
    // switch reads as one thing; it shares the gather, the snapshot and the
    // neighbours and diverges only in where it writes.
    std::size_t stamp_color(MeshBrush verb, const MeshBrushSettings& settings,
                            const SculptSnapshot& snapshot, const SculptNeighbors& neighbors,
                            VertexDeltas* record);
    SculptSnapshot snapshot_of() const;
    SculptNeighbors neighbors_of() const;
    // Flatten the region's one-rings into CSR. Built only for the verbs that
    // read neighbours, and the normals only for polish, which is the one verb
    // that reads a neighbour's own normal.
    void build_neighbors(bool want_normals, bool want_colors);
    // The compiled plan for this verb and these settings, recompiled only when
    // one of the three things it actually depends on changes.
    const BrushRuntimePlan& plan_for(MeshBrush verb, const MeshBrushSettings& settings);
    kernel::cfloat3 automask_reference(const MeshBrushSettings& settings);
    // The caller's seed if it is usable by THIS sculptor, `kNoClass` otherwise.
    // Counts the rejections it makes on revision grounds.
    std::uint32_t accepted_seed(const MeshBrushSettings& settings);
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
    // The walk's own output, in weld classes, before the composition turns it
    // into `WorkItemId`s. A member for the reason everything else here is one:
    // a stroke of similar stamps allocates on its first stamp and never again.
    std::vector<std::uint32_t> candidates_;
    std::vector<float> distance_;
    // The transients one stamp needs deep inside the call — the automask's
    // frontiers and its flood. See `brush_arena.h`.
    BrushScratchArena arena_;
    std::vector<kernel::cfloat3> displacement_;
    // Where each class's colour should END UP, seeded from what it holds now,
    // so a verb that leaves an entry alone writes nothing rather than
    // rewriting a colour with itself.
    std::vector<kernel::cfloat3> color_target_;
    // The pre-stamp colours the colour kernels read while writing
    // `color_target_`. A member, so a colour stroke does not allocate per dab.
    std::vector<kernel::cfloat3> color_current_;
    AutomaskInputs automask_inputs_;
    // The class the last gather anchored on, shared by the connectivity automask
    // and the normal-angle reference so the two cannot disagree about where the
    // brush landed.
    std::uint32_t automask_seed_ = kNoClass;
    // This sculptor's identity in the seed-token space, and how many seeds it
    // has turned away. Assigned once at construction; see `seed_revision`.
    std::uint64_t seed_revision_ = 0;
    std::size_t stale_seeds_rejected_ = 0;
    memory::PeakTelemetry* telemetry_ = nullptr;
    // The multi-pass kernels' buffers, reset rather than freed between stamps.
    SculptScratch scratch_;
    // The compiled plan and the three inputs it depends on. Not the whole
    // settings struct: radius, strength, centre and direction change on every
    // stamp of a stroke and change nothing about what the kernel needs.
    BrushRuntimePlan plan_;
    MeshBrush plan_verb_ = MeshBrush::Draw;
    int plan_iterations_ = 0;
    bool plan_geodesic_ = false;
    bool plan_valid_ = false;
    std::size_t plan_compilations_ = 0;
    // The region's one-rings, flattened. Kept as members for the same reason
    // everything else here is: a stroke of similar stamps must allocate on its
    // first stamp and never again.
    std::vector<std::uint32_t> nb_offsets_, nb_slots_;
    std::vector<kernel::cfloat3> nb_positions_, nb_normals_, nb_colors_;
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
