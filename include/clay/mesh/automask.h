#pragma once

// AUTOMASKING: the gates a brush applies to itself (brush-engine spec,
// add-shared-brush-kernels).
//
// A painted mask is something the artist DID. An automask is something the
// brush works out from the surface it landed on — do not cross onto a face
// pointing the other way, do not drag the mesh's open border, stay on the
// polygroup I started in, protect the crevices. Every one of them is the same
// shape: one scalar per vertex, multiplied into the weight.
//
// COMPOSED INTO THE WEIGHT, NEVER BRANCHED INTO A VERB. Sixteen verbs times
// five factors is eighty places to get a gate wrong; one multiplication is one.
// It is applied LAST of the weight's factors, which is not cosmetic — see
// `compose_weight`: a stamp with no automask multiplies by an identical 1.0 and
// lands on exactly the bits it landed on before automasking existed.
//
// THE FACTORS THIS FILE CANNOT COMPUTE, AND WHY THAT IS RIGHT. Cavity and
// curvature are a field's LAPLACIAN — `brush::measure_at` — and polygroups are
// a world lattice, `voxel::GroupField`. `mesh` may include neither module:
// `brush` and `voxel` both depend on `mesh`, so reaching either from here would
// be a cycle. So they arrive as callbacks the caller supplies, and
// `brush::apply_to_mesh` — the one place that can see all three — is where they
// are wired up.
//
// That constraint is doing real work rather than being worked around. The
// requirement is that a painted cavity mask and a cavity automask cannot
// disagree about one surface, and the way to guarantee that is to have exactly
// one estimator. `mesh` structurally CANNOT write a second one, so there is no
// second one to drift.

#include <cstdint>
#include <functional>

#include "clay/kernel/shim.h"
#include "clay/mesh/brush_arena.h"
#include "clay/mesh/work_item.h"

namespace clay {
namespace mesh {

struct Mesh;
class Adjacency;
struct SculptWorkset;

// Which gates are on. A bit set rather than a struct of bools, so a preset
// carries one integer and the loop asks one question per factor.
enum class AutomaskFactor : std::uint32_t {
    None = 0,
    // Fade out where the surface turns away from the brush's own facing. What
    // stops a stamp on the front of an ear from pulling the back of it.
    NormalAngle = 1u << 0,
    // Only the surface CONNECTED to where the brush landed. Free under a
    // geodesic footprint, which is already a walk; it costs a walk under a ball
    // footprint, which is where flatten and scrape live and where two surfaces
    // a hair apart are exactly the hazard.
    TopologyConnected = 1u << 1,
    // Fade out approaching an open border, so a brush near the rim of a patch
    // does not peel it.
    Boundary = 1u << 2,
    // Protect the crevices. Supplied by the caller — see the header note.
    Cavity = 1u << 3,
    // Stay inside the polygroup the brush started in. Supplied by the caller.
    SurfaceGroup = 1u << 4,
};

inline std::uint32_t operator|(AutomaskFactor a, AutomaskFactor b) {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
inline bool has_factor(std::uint32_t factors, AutomaskFactor f) {
    return (factors & static_cast<std::uint32_t>(f)) != 0;
}

// Where the connectivity flood starts, IN WORKSET SLOTS rather than in a
// representation's own identity — resolving an identity to a slot is the
// adapter's job, because `SculptWorkset::slot` is the adapter's array.
//
// THREE STATES RATHER THAN TWO, and the third is not a technicality.
// `resolved == false` means no seed could be found at all, and the factor does
// not run: a flood with nowhere to start must not mask a stamp the caller never
// asked to have masked. `resolved` with `slot == kNoClass` means a seed WAS
// found and did not survive the falloff into the workset, and that masks
// everything — a flood from a seed that is not there reaches nothing, and
// saying so is the honest answer where quietly passing everything would not be.
struct ConnectivitySeed {
    bool resolved = false;
    std::uint32_t slot = 0xffffffffu;  // kNoClass; see sculpt_common.h
};

struct AutomaskSettings {
    std::uint32_t factors = 0;

    // NormalAngle: how far the surface may turn from the brush's facing before
    // the gate closes, in radians. Full strength up to this angle, zero at
    // twice it — the same two-sided shape polish's own gate uses, and for the
    // same reason: a gate that steps from 1 to 0 across one edge leaves a bead.
    float normal_angle = 1.0471976f;  // 60 degrees

    // Boundary: how many rings of fade to leave at an open border. 0 is a hard
    // stop at the border itself.
    int boundary_rings = 2;

    // Cavity: how much of the caller's cavity measure to apply. 1 masks a full
    // crevice completely; 0 is off even when the factor bit is set, which is
    // what a host's slider at zero should cost.
    float cavity_strength = 1.0f;

    bool any() const { return factors != 0; }
};

// The two factors `mesh` may not compute for itself. Set once per STROKE rather
// than per stamp — they hold `std::function`s, and copying those per dab is an
// allocation per dab, which is exactly what the allocation gate forbids.
struct AutomaskInputs {
    // The cavity measure at a world point, in [0,1]; 1 is a full crevice.
    // Wired to `brush::measure_at` with `SurfaceMeasure::Cavity`.
    std::function<float(kernel::cfloat3)> cavity;
    // The surface group at a world point. Wired to the document's group field,
    // which is addressed on a WORLD LATTICE rather than per face — that is what
    // makes a group survive a representation bridge, and a per-face copy would
    // be a second answer to the same question.
    std::function<std::uint32_t(kernel::cfloat3)> group;
    // The group the stroke started in. Anything else is masked out.
    std::uint32_t active_group = 0;

    bool empty() const { return !cavity && !group; }
};

// One scalar per WORKSET entry, written into `out`, which the caller sizes.
//
// Per workset, never per mesh: an automask must cost what the stamp touched.
// A factor that is off contributes nothing and is not evaluated.
//
// `reference_normal` is the brush's own facing, fixed for the whole stamp. It
// is NOT recomputed from the region, which would be circular — the automask is
// shaping the very weights the region's average normal is weighted by.
//
// THE NEUTRAL CORE. It names no representation: three of the five factors need
// none at all (NormalAngle reads `workset.normals`; Cavity and SurfaceGroup
// call the caller's own functions on `workset.positions`), and the two that do
// — the boundary fade and the connectivity flood — ask `WorkItemTopology` the
// two questions in `work_item.h` and get their answers in workset slots.
//
// That split is the whole of why an automask an artist enables now reaches the
// adaptive surface: before it, `compute_automask` took a `Mesh` and an
// `Adjacency`, so the one representation that has neither could not call it,
// and `DynamicSculptor::gather` silently dropped the four automask fields the
// descriptor had already carried to it.
//
// The arena carries the five per-stamp arrays this used to build as
// `std::vector`s. Each has an exact and knowable bound: a slot enters a
// breadth-first frontier at most once, and a ring cannot hold more distinct
// in-workset neighbours than the workset has entries.
void compute_automask(const WorkItemTopology& topology, const SculptWorkset& workset,
                      const AutomaskSettings& settings, const AutomaskInputs& inputs,
                      kernel::cfloat3 reference_normal, ConnectivitySeed seed,
                      BrushScratchArena& arena, float* out);

// THE FIXED MESH'S ADAPTER, kept under the signature it always had so a caller
// holding a mesh and an adjacency does not have to build a topology to ask.
// `seed_class` is a weld class and is resolved to a workset slot here.
void compute_automask(const Mesh& mesh, const Adjacency& adjacency, const SculptWorkset& workset,
                      const AutomaskSettings& settings, const AutomaskInputs& inputs,
                      kernel::cfloat3 reference_normal, std::uint32_t seed_class,
                      BrushScratchArena& arena, float* out);

// The fixed mesh's `WorkItemTopology`: a ring is the adjacency's ring, and an
// open border is a ring neighbour sharing exactly one triangle.
//
// Declared here rather than hidden in the implementation because the fixed
// sculptor's own gather passes one to `compose_workset` directly — the automask
// is one step of a composition rather than a call the sculptor makes on its
// own.
class MeshWorkItemTopology final : public WorkItemTopology {
   public:
    MeshWorkItemTopology(const Mesh& mesh, const Adjacency& adjacency,
                         const SculptWorkset& workset)
        : mesh_(mesh), adjacency_(adjacency), workset_(workset) {}

    void ring_slots(std::uint32_t slot, ScratchVector<std::uint32_t>* out) const override;
    bool on_open_border(std::uint32_t slot) const override;

   private:
    const Mesh& mesh_;
    const Adjacency& adjacency_;
    const SculptWorkset& workset_;
};

// Whether a class sits on an open border: it has a ring neighbour with which it
// shares exactly one triangle. Exposed because the boundary gate is not the
// only thing that wants to know.
bool is_boundary_class(const Mesh& mesh, const Adjacency& adjacency, std::uint32_t cls);

}  // namespace mesh
}  // namespace clay
