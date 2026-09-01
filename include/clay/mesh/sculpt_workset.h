#pragma once

// The WORKSET: everything one stamp reads, and the record of what it wrote
// (meshing spec, add-shared-brush-kernels, add-shared-brush-runtime).
//
// A stamp costs what it touches. That rule is the whole reason this type is a
// reusable object rather than a set of locals: its arrays are sized to the
// largest footprint the stroke has seen and cleared through their own contents,
// never wholesale, so nothing here is ever proportional to the surface.
//
// THE READ HALO IS NOT THE WRITE REGION, and keeping them apart is the point of
// the second half of this file. Smooth, Relax and Polish average over a
// one-ring, so they READ vertices they never MOVE. A dirty report that named
// the halo would make a host re-upload geometry that did not change — on a
// multi-million-vertex surface at pointer rates, that is the difference between
// a preview that keeps up and one that does not.
//
// Shared rather than per-sculptor because the adaptive and multiresolution
// sculptors gather a workset too, and three answers to "what did this stamp
// touch" is three chances for a host to be told the wrong one.
//
// IT ADDRESSES WORK BY `WorkItemId`, NOT BY WELD CLASS. A weld class is the
// FIXED mesh's identity; the adaptive surface's is a `VertexId` and the
// hierarchy's is (level, vertex). Typed in weld classes, this was a workset
// only one sculptor could fill — see `work_item.h` for why the neutral identity
// is 64 bits and why it is not a template.
//
// NOTHING HERE MAY NAME A `Mesh`, AN `Adjacency`, A `Bvh` OR A VERTEX INDEX,
// and that is a rule about this file rather than an observation about it. It is
// also why the three `build_*_workset` adapters are NOT declared here: each is
// declared in the header of the representation it serves, because a neutral
// header holding three representation-specific signatures is neutral in the
// directory listing only. What IS declared here is `compose_workset` — the one
// step all three walks end in, and the one whose duplication is the defect.

#include <cstdint>
#include <vector>

#include "clay/field/relax.h"  // MaskGate
#include "clay/kernel/shim.h"
#include "clay/math/geom.h"
#include "clay/mesh/brush_arena.h"
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/sculpt_kernels.h"  // AlphaFrame, WeightFactors
#include "clay/mesh/work_item.h"

namespace clay {
namespace mesh {

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
struct SculptWorkset {
    std::vector<WorkItemId> items;           // what the falloff reached
    std::vector<float> weights;              // the composed weight, in [0,1]
    std::vector<kernel::cfloat3> positions;  // pre-stamp, one per item
    std::vector<kernel::cfloat3> normals;    // pre-stamp, unit

    // The automask factors, composed. Sized only when a stamp has any: an
    // automask must cost the workset and never the surface, and an empty vector
    // is how "this stamp has none" is said without a per-vertex branch.
    std::vector<float> automask;

    kernel::cfloat3 average_normal = kernel::cf3(0, 1, 0);
    kernel::cfloat3 centroid = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    bool empty() const { return items.empty(); }
    std::size_t size() const { return items.size(); }

    // item identity -> index into `items`, kNoClass outside the region. Sized
    // to the REPRESENTATION's own count and reset over `items` alone, so a
    // stamp costs what it reached rather than what the surface holds.
    //
    // It is also how the READ HALO is expressed: a ring neighbour whose slot is
    // `kNoClass` is one the stamp reads and does not move.
    //
    // IT IS THE ADAPTER'S ARRAY. Its size belongs to the representation — the
    // class count, the vertex pool's slot count, the level's vertex count — so
    // only the walk that filled the workset can size it or reset it. The
    // neutral composition PUBLISHES into it and reads nothing back through it,
    // and it can do that much because `WorkItemId::key()` is the dense half of
    // every one of the three identities. Neighbour lookups go through
    // `WorkItemTopology` instead, which answers in slots.
    std::vector<std::uint32_t> slot;

    // -- what the stamp actually WROTE ---------------------------------------
    //
    // A subset of `items`: the ones whose displacement was not zero. The rim
    // of a falloff, a fully masked vertex and a verb that declined all leave
    // entries in `items` that never move, and a host told about those would
    // upload them for nothing.
    std::vector<WorkItemId> write_region;
    // The bounds of what moved, in the mesh's own space. Empty when nothing
    // did.
    math::Aabb write_bounds;

    // Clear the per-stamp arrays and KEEP their storage. `slot` is not cleared
    // here — it is a per-identity array reset through `items` by the gather,
    // which is the only reset that costs the footprint rather than the surface.
    void clear_keep_capacity() {
        items.clear();
        weights.clear();
        positions.clear();
        normals.clear();
        automask.clear();
        write_region.clear();
        write_bounds = math::Aabb{};
    }
};

// The name this type had when it was only the fixed sculptor's. Kept so that
// nothing outside has to change, and because "the region under the brush" is
// still what it is.
using BrushRegion = SculptWorkset;

// -- the one neutral step -----------------------------------------------------

// The two questions the composition cannot answer for itself, because both
// answers live in the representation.
//
// A BORROWED FUNCTION POINTER AND A CONTEXT, never a `std::function`. This is
// called once per kept candidate on a per-dab path; a `std::function` holding a
// sculptor pointer and nothing else would fit its small buffer today and would
// allocate on the day somebody captured a second thing — a per-dab allocation
// arriving from a change that looked like it captured one more variable. A
// function pointer cannot develop that.
struct WorkItemReader {
    // The item's pre-stamp normal. Called only for candidates the falloff
    // KEPT, which is what lets the fixed mesh's angle-weighted `class_normal`
    // — a pass over the class's incident triangles — stay off the rim entries
    // that were about to be dropped.
    kernel::cfloat3 (*normal_at)(const void* context, WorkItemId item) = nullptr;
    // The freeze the representation carries on the ITEM itself, as opposed to
    // the caller's world-space gate. Null when there is none, and null is not
    // the same as a function returning zero: the adaptive surface takes
    // `max(gate, vertex mask)` because two independent freezes are not half a
    // freeze each, and a representation with no vertex mask must not have that
    // `max` evaluated at all, or its stamps stop being bit-identical to the
    // ones it took before this call existed.
    float (*mask_at)(const void* context, WorkItemId item) = nullptr;
    const void* context = nullptr;
};

// Everything `compose_workset` needs that is not already in the workset.
struct WorkComposeInputs {
    const MeshBrushSettings* settings = nullptr;
    // The caller's freeze. May be an empty `MaskGate`, which costs nothing.
    const field::MaskGate* gate = nullptr;
    // The stamp's alpha basis, resolved once by the walk — never null; a stamp
    // with no alpha passes a default-constructed one and `alpha_at` returns an
    // exact 1.
    const AlphaFrame* alpha = nullptr;

    // The walk's own along-surface distance, one per CANDIDATE, in world units.
    // Read only when `geodesic`; a ball footprint has no walk and passes null.
    const float* path_distance = nullptr;
    bool geodesic = false;
    // The walk's policy, passed rather than read from a constant here, for the
    // reason `path_taper` gives: these belong to the walk, the walk lives in
    // `adjacency.h`, and this header may not name that file.
    float taper_start = 1.0f;
    float path_budget = 1.0f;

    // -- the automask ---------------------------------------------------------
    // Null topology or null inputs means the automask does not run, whatever
    // the settings say. A representation that cannot answer the two topological
    // questions says so by passing null rather than by returning ones, which
    // would be indistinguishable from an automask that had nothing to mask.
    const WorkItemTopology* topology = nullptr;
    const AutomaskInputs* automask_inputs = nullptr;
    // The brush's own facing, fixed for the stamp. NOT the region's average
    // normal, which would be circular — the automask is shaping the very
    // weights that average is weighted by.
    kernel::cfloat3 automask_reference = kernel::cf3(0, 1, 0);
    // Where the connectivity flood starts, as an ITEM. Null when the caller
    // could not resolve one, which switches the factor off rather than flooding
    // from an arbitrary entry. It is turned into a workset slot here, after the
    // slot map is published — see `ConnectivitySeed` for the three states.
    const WorkItemId* automask_seed = nullptr;

    WorkItemReader reader;
};

// THE STEP ALL THREE WALKS END IN.
//
// On entry the workset holds `items` and `positions` for every CANDIDATE the
// walk produced, and `slot` is sized to the representation and holds `kNoClass`
// for every candidate. On return it holds only the entries that survived, with
// their weights, normals and automask factors, `slot` republished over them,
// and the stamp's frame resolved. Returns how many survived.
//
// What it does, in the one order that is the contract:
//
//   1. the five weight factors, composed by `compose_weight` — falloff, path
//      taper, (1 - gate), alpha, automask — with the automask still 1;
//   2. drop everything that reached zero, compacting in place;
//   3. publish `slot`;
//   4. the automask, through the supplied topology, as a SECOND pass — two of
//      its five factors spread over the workset's own neighbourhood, so they
//      cannot be answered one entry at a time while the workset is still being
//      built;
//   5. drop again, so a fully masked entry leaves the workset ENTIRELY and is
//      bit-identical to its input rather than merely close, and republish;
//   6. resolve `average_normal`, `centroid`, `plane_point` and `plane_normal`
//      from the snapshot and never from what the stamp is about to deposit.
//
// THE FACTOR ORDER IS THE CONTRACT, not an implementation detail: these are
// separate multiplications, float multiplication is not associative, and
// re-associating them moves the last bit of every displacement that reads the
// weight. That is why this is one function rather than three that agree.
//
// WHAT IT DELIBERATELY DOES NOT DO IS THE WALK. The walk IS the
// representation — weld classes through an `Adjacency`, half-edges through a
// pool whose slots retire under it, and a hierarchy that does not walk at all
// but delegates to its bound level. Unifying three working walks against a
// golden that must not move would buy no shared line at the end of it.
std::size_t compose_workset(const WorkComposeInputs& inputs, BrushScratchArena& arena,
                            SculptWorkset* workset);

}  // namespace mesh
}  // namespace clay
