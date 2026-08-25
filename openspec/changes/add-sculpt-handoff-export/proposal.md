# Proposal: emit the sculpt handoff CyberRemesher already reads

## Why

`add-claycore-bridge` asked whether CyberRemesherAndUV needs anything ClayCore
does not already emit. Answered by reading that repository rather than guessing,
and the answer is yes — with a format already written down and a reader already
shipped.

`CyberRemesherAndUV/docs/sculpt-handoff-format.md` says so plainly:

> **Status: defined unilaterally by this repository.** The handoff was specified
> here, and this repo ships the *reading* half only. Agreement with ClayCore's
> export-profile change — "one shared document, two implementations" — is
> **outstanding**; ClayCore is not present in this tree and no negotiation has
> taken place.

Their CLI already assumes our half exists:

```sh
producer --for-retopo | cyberremesh --target - --output low.obj --preset blender
```

So this is not a documentation change. The other side has been waiting.

## What is missing, measured against their spec

| their required payload | ClayCore today |
|---|---|
| positions + triangle connectivity | present |
| `nx ny nz` per vertex | the PLY writer emits them **when the mesh has them**, and a marching-cubes mesh only has them if gradients were requested |
| `red green blue`, uchar 0-255 | present |
| **`material_mix`**, float 0-1 | **no such concept exists** |
| `comment cyber_sculpt_handoff 1 0` | no way to write a comment at all |
| `comment cyber_handoff_producer <label>` | same |

Two more that their reader enforces and our writer would violate:

- **Faces must be triangles.** `save_ply` declares a mesh's QUADS as the faces
  when it has them, and their reader rejects any face of another arity —
  *"a sculpt export that is not triangulated is a producer bug"*. Our best
  export, the quad mesh, is exactly the one their reader would refuse.
- **Normals are required**, and a mesh meshed without gradients has none. A
  handoff missing them is not a handoff.

## What changes

**A handoff writer**, in both profiles their reader accepts:

- the **PLY file profile**, which is their normative one, and
- the **in-memory buffer profile**, for when both engines run in one process —
  which is the case that matters on a tablet, where writing a file to hand a
  mesh to a library in the same address space would be absurd.

The buffer profile costs almost nothing here because the arrays are already
exposed: positions, normals, colours and indices are borrowed pointers today.
Only `material_mix` has to be produced, so this change adds that one channel and
does NOT duplicate a struct the other repository owns.

**`material_mix` comes from a mask.** ClayCore has no material slots and this
change does not invent them. A mask is already a per-vertex-resolvable scalar in
`[0, 1]` that an artist painted, which is exactly the shape and the meaning
their spec asks for — the host names the mask that means "the second material".
With no mask the channel writes zeros, which is the honest answer for a document
that never expressed one.

**Normals and triangulation are guaranteed by the writer**, not assumed of the
caller: normals are computed when absent, and the faces written are always the
triangle list even for a mesh carrying quads.

## What this is not

- **Not accepting their format as permanent.** It is version 1.0, defined
  unilaterally, and this change records that. If `material_mix` should have been
  optional for a producer with no material slots, that is a conversation for a
  minor bump; emitting it costs us one float per vertex and unblocks the
  pipeline today.
- **Not baking.** The seam decision (`add-claycore-bridge` task 1.6) was made
  separately: ClayCore answers field queries and does not learn UV semantics.
  Their engine bakes.
- **Not a new mesh format.** The handoff is a PLY with two comment lines and one
  extra vertex property. Nothing else in `io` changes.
