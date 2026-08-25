#pragma once

// THE SCULPT HANDOFF: sculpt -> retopology -> UV -> bake, without either engine
// linking the other (add-sculpt-handoff-export).
//
// The format is NOT defined here. It is CyberRemesherAndUV's
// `docs/sculpt-handoff-format.md`, version 1.0, and that repository ships the
// READING half — it says so, and says the agreement with ClayCore is
// outstanding because no negotiation ever took place. Their CLI already assumes
// this half exists:
//
//     producer --for-retopo | cyberremesh --target - --output low.obj
//
// So this file is the other half of a document written elsewhere. Where their
// spec and this header disagree, their spec is right and this is a bug.
//
// TWO GUARANTEES THE WRITER MAKES ON THE CALLER'S BEHALF, because both are
// conditions their reader enforces and neither is something a caller can be
// expected to know:
//
//  - THE FACES ARE ALWAYS TRIANGLES. `save_ply` declares a mesh's QUADS as its
//    faces when it has them, and their reader rejects any face of another
//    arity — "a sculpt export that is not triangulated is a producer bug". Our
//    best export, the quad mesh, is exactly the one it would refuse.
//  - NORMALS ARE ALWAYS PRESENT. They are required, and a mesh meshed without
//    gradients has none.
//
// WHAT `material_mix` IS HERE. Their spec calls it "the sculpt's per-vertex
// blend weight between two material slots". ClayCore has no material slots and
// this does not invent them: the channel is resolved from a MASK, which is
// already a painted scalar in [0,1] answerable at any point — the shape and the
// meaning their channel asks for. The host names the mask that means "the
// second material". With no mask it is zero for every vertex, which is the
// honest answer for a document that never expressed one.
//
// Version 1.0 is accepted AS WRITTEN. If it is ever negotiated, the one field
// that cost a decision rather than a line of code is `material_mix` being
// required of a producer with no material slots.

#include <cstdint>
#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/mesh/mesh_data.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace io {

// Their `cyber::handoff::kVersionMajor` / `kVersionMinor`. A reader rejects a
// newer minor rather than dropping what it does not know, so these move only
// when their spec does.
inline constexpr int kHandoffVersionMajor = 1;
inline constexpr int kHandoffVersionMinor = 0;

struct HandoffOptions {
    // Free-form, surfaced in their CLI report. Empty omits the comment.
    std::string producer = "claycore";
    // The mask whose value becomes `material_mix`, resolved at each vertex.
    // Null writes zeros — the payload is still present, because it is required.
    const voxel::MaskField* material_mask = nullptr;
    bool binary = true;
};

// The PLY profile, which their spec calls the normative one.
//
// A standard PLY carrying two comment lines and one extra vertex property. The
// mesh is NOT modified: normals are computed into the output when absent.
std::vector<std::uint8_t> save_handoff_ply(const mesh::Mesh& m, const HandoffOptions& options = {});
IoStatus save_handoff_ply_file(const mesh::Mesh& m, const std::string& path,
                               const HandoffOptions& options = {});

// The in-memory buffer profile is deliberately NOT a struct here.
//
// Their `cyber::handoff::BufferView` wants positions, normals, colours,
// material mix and indices. Four of those five are already borrowed pointers on
// a `mesh::Mesh`, so a struct of ours would duplicate one they own and give two
// places for the two engines to disagree about the layout. This produces the
// one array they cannot get from what we already expose.
//
// Sized to the mesh's vertex count. `mask` null gives zeros.
std::vector<float> handoff_material_mix(const mesh::Mesh& m, const voxel::MaskField* mask);

}  // namespace io
}  // namespace clay
