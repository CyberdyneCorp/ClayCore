#pragma once

// A lattice (free-form deformation) cage over a mesh layer — ZBrush's Gizmo
// Lattice, Blender's Lattice modifier.
//
// THIS RUNS FORWARD, and that is the whole reason it exists here rather than
// as an SDF deformer. A claycore SDF deformer is an INVERSE point map: it
// answers "where did the material at p come from", because evaluating an
// implicit field means asking about an arbitrary query point. Forward FFD has
// no closed-form inverse, so a lattice on an SDF item needs one of three
// compromises (see #116) and remains open.
//
// A mesh has no such problem: it already knows where its vertices are. So the
// cage is applied the way Blender and ZBrush apply theirs — find a vertex's
// parameters in the cage, evaluate, move it — with no inversion, no iteration
// and no approximation.
//
// OFFSETS, NOT POSITIONS. The cage stores how far each control point has been
// dragged from where it started, so a cage nobody has touched is EXACTLY the
// identity at every point rather than approximately one. The warp is
//
//     new_position = p + Bernstein(offsets, clamp(s, t, u))
//
// which also settles what happens outside the box: the clamp means a vertex
// out there picks up the offset of the nearest point of the cage and travels
// rigidly with it. Evaluating a POSITION cage at a clamped parameter would
// instead drag every outside vertex onto the box's surface, which is the
// mistake this formulation exists to avoid. Holding beyond the span is the
// convention `twist_range` and `bend_range` already use.

#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/math/geom.h"

namespace clay {
namespace mesh {

// A cage needs at least two control points on an axis to span it, and past
// this a cage is a modelling tool nobody is driving by hand.
inline constexpr int kMinLatticeDivisions = 2;
inline constexpr int kMaxLatticeDivisions = 32;

class Lattice {
  public:
    // A cage over `box` with `nx * ny * nz` control points on a regular grid,
    // every offset zero. Divisions are clamped into
    // [kMinLatticeDivisions, kMaxLatticeDivisions]; an empty box gives a cage
    // that is the identity and stays that way, since it has nothing to span.
    Lattice(const math::Aabb& box, int nx = 3, int ny = 3, int nz = 3);

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    std::size_t point_count() const { return offsets_.size(); }
    const math::Aabb& box() const { return box_; }

    // How far a control point has been dragged, and where it started. `rest`
    // is derived from the box rather than stored, so the two cannot disagree.
    kernel::cfloat3 offset(int i, int j, int k) const;
    void set_offset(int i, int j, int k, kernel::cfloat3 v);
    kernel::cfloat3 rest(int i, int j, int k) const;
    // Where the control point is now — what a UI draws.
    kernel::cfloat3 position(int i, int j, int k) const { return rest(i, j, k) + offset(i, j, k); }

    // Every offset zero: the identity, and worth asking before walking a mesh.
    bool is_identity() const;

    // The displacement this cage applies at a world point. Zero everywhere for
    // an untouched cage, exactly.
    //
    // An axis on which the box is FLAT — which is what a cage over a plane's
    // own bounds gives — reads as the middle rather than as an end, so none of
    // the control points on it are dead. See the note in the .cpp.
    kernel::cfloat3 displacement(kernel::cfloat3 p) const;

  private:
    std::size_t index(int i, int j, int k) const;

    math::Aabb box_;
    int nx_ = 3, ny_ = 3, nz_ = 3;
    std::vector<kernel::cfloat3> offsets_;
};

}  // namespace mesh
}  // namespace clay
