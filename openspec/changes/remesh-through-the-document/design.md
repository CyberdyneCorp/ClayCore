# Design: remesh-through-the-document

## Decisions

### 1. A new history step kind, not a command and not a delta

`scene::Command` cannot express it: a mesh layer's triangles live in
`io::ClaySpaceDoc::mesh_layers`, beside the document rather than in the edit
list, and the command vocabulary reaches the edit list. `VertexDeltas` cannot
express it either, and that is the fixed-topology contract working as intended —
a delta records no indices, so it cannot describe a change that replaces them.
`TopologyDelta` records the split, collapse and flip an adaptive edit made, and
a rebuild from a volume made none of them.

So `Step::Kind::MeshReplace`, holding both meshes. A SNAPSHOT where the mesh and
voxel kinds hold diffs, and the precedent is `SurfaceGroup`, which made the same
choice for the same two reasons: there is nothing to diff against, and the edit
happens a handful of times a session rather than hundreds of times a second.

It is by a wide margin the largest step there is, which is exactly why
`step_bytes` had to learn it. A budget blind to the biggest kind is not a
budget.

### 2. The revision is on the layer, not on the mesh

A revision describes a LAYER's history. `mesh::Mesh` is the interchange type —
an owned mesh belongs to no layer, cannot be replaced under anyone, and would
carry a field that never means anything. So the counter lives on the document
handle, keyed by layer id, in both bindings.

**It moves for a replacement and not for a sculpt**, and that asymmetry is the
whole value. A brush moves vertices and leaves `indices` byte-identical, which
is what lets an `Adjacency`, a `Bvh` and a `MeshSculptor` stay valid across a
stroke; a rebuild invalidates all three. A counter that moved for both would
force a rebuild after every dab.

### 3. Three checks, and only the third catches the case that matters

`resolve_sculptor` already compared the layer's mesh POINTER and the sculptor's
vertex and index COUNTS. Both pass a replacement that lands on the same counts —
a `std::map` node's address is stable across an assignment — and the sculptor
is then holding an adjacency and a BVH over triangles that no longer exist.

The regression test builds exactly that case: the same positions with the index
array reversed. Same counts, different connectivity. With the revision check
removed it stamps successfully into the stale index; with it, the stamp is
refused. That is what makes it a test rather than a description.

### 4. Projection weights rather than rejects — a measured correction

`add-voxel-remesher` shipped a hard reject on normal incompatibility, and its
header asserted the danger confidently: "a jump is not a small error, it is a
hole pulled through the surface." **That claim was never tested and does not
survive being tested.**

The fixture that reaches the branch is a sheet folded back through itself, where
about a fifth of the vertices inside the clamp have a back-facing closest point.
Measured there at longest-axis 96:

| | surface distance | self-intersecting pairs |
|---|---|---|
| no projection | 0.38643 | 0 |
| project, no test | 0.38617 | 0 |
| project, hard reject | 0.38637 | **17** |
| project, weighted | 0.38642 | 0 |

The hard reject is the only variant that makes the surface worse, and the
mechanism is plain: moving a vertex fully while leaving its neighbour untouched
is a discontinuous displacement, and a discontinuous displacement tears. The
weight goes to zero continuously and does not.

The specified behaviour is unchanged where it was specified — a source facing
away still moves the vertex not at all — so this is a correction inside the
contract rather than a change to it.

### 5. What this change does NOT fix, and says so

`DynamicSurface::from_mesh` refuses ANY marched mesh at its default weld
epsilon. A plain `mesh_lattice` over an analytic sphere emits 486 edges shorter
than 1e-5 and is refused with `DegenerateTriangle`; so is `mesh_tape`, the
ordinary document meshing path; so is a voxel remesh, which emits ten.

The correct option for a marched mesh is `weld_epsilon = 0`, and that is not a
workaround: the marcher's builder already welded it on canonical lattice-edge
keys, so welding again by DISTANCE merges vertices it deliberately kept apart.
The default is right for an IMPORTED mesh whose exporter duplicated a seam.

No caller in the repository had ever converted a marched mesh to an adaptive
surface — the dynamic-topology example builds its input analytically — which is
why this had not surfaced. Documenting the option is this change's scope;
deciding whether `from_mesh` should repair, refuse or default differently is
that subsystem's, and wants its own change.

## Open questions

- **Whether the journal should carry the whole mesh.** It does, because a
  recovery that silently lacked a rebuild would be worse. But a two-million
  triangle rebuild is a large journal entry, and a host that trims aggressively
  may prefer a barrier. Not decided against; not needed yet.
- **Whether the revision should live in the file.** It does not, so a reloaded
  document starts every layer at 1. That is correct for the invalidation use —
  nothing cached survives a reload — and would be wrong if a revision ever had
  to mean something across sessions.
