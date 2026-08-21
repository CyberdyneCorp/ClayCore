# Proposal: paint and smear on a mesh layer

## Why

`MeshSculptor` has fourteen verbs and every one of them moves vertices.
Nothing writes `Mesh::colors`. A mesh layer can CARRY imported vertex colours
and export them, but cannot have them edited — which makes it the odd
representation out, now that the SDF side paints through `Op::Paint` strokes
and the voxel side through its palette.

Blender's **Paint** and **Smear** are the missing pair, and they are the last
named gap in the mesh brush vocabulary. `decide-surface-colour` named them and
deliberately left them, pinning the omission with a test that every verb leaves
`colors` byte-identical — so that adding one would have to be a deliberate act
rather than an accident. This is that act.

## What changes

Two verbs, `Paint` and `Smear`, and the smallest amount of surrounding
machinery that makes them honest.

**Paint** blends each vertex's colour toward a target by the brush's own
per-vertex weight — the same weight every other verb uses, so falloff,
strength, the geodesic walk, the mask gate and the alpha stamp all compose with
it without a line of per-verb code.

**Smear** pushes existing colour along the drag direction. It samples from the
one-ring rather than through the BVH: for each vertex it blends toward the
neighbour lying most nearly OPPOSITE the drag, weighted by how well that
neighbour lines up. Topology is fixed here, so the one-ring is a complete and
cheap description of "where the colour just came from", and it needs no spatial
query and no interpolation scheme to argue about.

### Three decisions worth stating

**A colour brush does not move vertices.** This is the exact mirror of the
property `decide-surface-colour` pinned, and it is worth pinning in the same
way: `positions` and `normals` come out byte-identical from a Paint or a Smear.
Any host can then run a colour pass over a finished sculpt without a diff on
the geometry.

**The colour attribute must already exist.** A mesh with no `colors` is
refused rather than silently given one. Allocating twelve bytes per vertex on
the first dab hides a real cost behind a brush stroke, and — worse — makes "I
painted and nothing happened" indistinguishable from "this mesh had no colour
attribute". `MeshSculptor::ensure_colors(fill)` creates it as an explicit act,
and the C boundary refuses with a message that names the fix.

**Undo has to learn a third channel.** `VertexDeltas` records positions and
normals; a colour brush that no record can revert would be the only verb in the
vocabulary that cannot be undone. Colours are recorded exactly the way normals
already are — captured on first touch, coalesced per gesture, restored rather
than recomputed — so a Paint stroke reverts bit-identically like every other.

## Impact

Additive at the engine, and additive at the C ABI in the way the versioned
descriptor pattern is designed for: `clay_mesh_brush_desc` grows a trailing
`color`, so a caller compiled against the older layout passes the shorter
descriptor and gets exactly the fourteen verbs it had. That growth is now
SAFE to make, which it was not two changes ago — `bound-output-descriptor-fills`
and `require-struct-size-on-defaults` are what make appending a field to a
descriptor a routine act rather than a latent overrun.

ABI 0.36.0.
