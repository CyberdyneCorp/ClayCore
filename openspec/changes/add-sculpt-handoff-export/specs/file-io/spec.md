# file-io

## ADDED Requirements

### Requirement: A mesh can be written as a sculpt handoff
The library SHALL write a mesh in the sculpt handoff profile that CyberRemesherAndUV's reader accepts, so that a sculpt can enter a retopology, UV and bake pipeline without either engine linking the other.

The handoff SHALL be available as a FILE and as IN-MEMORY BUFFERS, since both engines may run in one process — on a tablet especially, where writing a file to hand a mesh to a library in the same address space is not a reasonable step.

The writer SHALL declare the handoff version and MAY declare a producer label, since a reader that cannot tell a handoff from an ordinary mesh of the same format has no way to apply the version gate.

#### Scenario: A written handoff declares its version
- **WHEN** a mesh is written as a handoff
- **THEN** the result declares the handoff version, and a reader can distinguish it from an ordinary mesh file of the same format

#### Scenario: Every required payload is present
- **WHEN** a mesh is written as a handoff
- **THEN** positions, per-vertex normals, per-vertex colours and a per-vertex material mix are all present

### Requirement: A handoff is always triangulated and always carries normals
The handoff writer SHALL write the triangle list as the faces, even for a mesh that also carries quads, and SHALL write per-vertex normals whether or not the mesh already had them.

Both are guarantees of the WRITER rather than requirements on the caller, because both are conditions the reader enforces and a caller cannot be expected to know. A mesh produced by quad export carries quads and is the export most likely to be handed over; a mesh meshed without gradients carries no normals. Handing either to the reader unchanged produces a rejected file for a reason the caller did nothing to cause.

Computing normals SHALL NOT modify the mesh being written.

#### Scenario: A quad mesh is handed over as triangles
- **WHEN** a mesh carrying quads is written as a handoff
- **THEN** the faces written are triangles, and the surface they describe is the same one the quads described

#### Scenario: A mesh without normals gains them
- **WHEN** a mesh with no normals is written as a handoff
- **THEN** the handoff carries per-vertex normals, and the source mesh is unchanged

### Requirement: The material mix comes from a mask, or is zero
The handoff's per-vertex material mix SHALL be derived from a MASK when the caller names one, by resolving the mask at each vertex position, and SHALL be zero for every vertex otherwise.

A mask is already a painted scalar in `[0, 1]` resolvable at any point, which is the shape and the meaning the handoff asks of this channel. The library SHALL NOT introduce material slots to satisfy it: a document that never expressed a material mix has none, and zero is the honest answer rather than an invented one.

#### Scenario: A masked region reports its mix
- **WHEN** a handoff is written with a mask that covers part of the mesh
- **THEN** vertices inside the painted region carry the mask's value and vertices outside it carry zero

#### Scenario: No mask means no mix
- **WHEN** a handoff is written without a mask
- **THEN** the material mix is zero for every vertex, and the payload is still present
