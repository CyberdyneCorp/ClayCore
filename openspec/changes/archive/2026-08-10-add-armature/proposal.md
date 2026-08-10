# Proposal: armatures — a tree of spheres that skins to a form

## Why

Blocking out a figure is the one thing this engine makes hard, and
`examples/34_organic_character.py` is the evidence. A humanoid is forty-odd
primitives whose positions are hand-written coordinates, each one placed by
guessing a number, rendering, and adjusting. There is no way to say "an arm
hangs from this shoulder" — only to compute where the arm's capsule endpoints
would be if it did.

ZBrush's ZSpheres are the standard answer, and the reason they work is not the
skinning. It is that the armature is a **tree**: a sphere knows its parent, so
moving a shoulder carries the whole arm, and adding a finger is one click on a
hand rather than three coordinates in a file. The mesh is a consequence.

## What this engine already has, which is most of it

`ctape_stroke` is a **chain** of sphere-swept segments. Each point is
`(x, y, z, radius)`; consecutive points are joined by a round cone — a capsule
when the radii match, a sphere when they coincide — and the segments are
combined with a smooth union whose radius is `stroke_blend_k`:

```
for (int i = 0; i + 1 < count; ++i) {
    ... sd_round_cone_ab(p, a, b, ra, rb) ...
    d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
}
```

That is a ZSphere armature with one restriction: the topology is a line. A
node's neighbour is the next one in the array, so nothing can branch.

**So the primitive is a generalisation, not a new subsystem.** Carry a parent
index per node and loop over `(i, parent[i])` instead of `(i, i + 1)`. The
segment maths, the smooth union, the blend parameter and the exactness
bookkeeping are unchanged. A chain-shaped armature must then produce a field
identical to the stroke it came from, which is the compatibility test that
keeps the two honest.

The rest of the brief's pipeline is also already here, and the proposal's job
is to say so rather than re-invent it:

- **"Preview in real time"** — a field needs no preview mesh. It is traced
  directly by `raycast_many`, which is what every example already renders with.
- **"Adaptive Skin (final generation)"** — `Document.mesh(resolution, mesher=…)`
  with marching cubes, surface nets or dual contouring. Already shipped.
- **"Partial update"** — a field is evaluated per point; there is no cached
  geometry per link to invalidate. The brick cache already does the incremental
  work, keyed on what actually moved.
- **Symmetry** — `layer.mirror("x")` with `mirror=True`, already used by
  example 34 for exactly this.
- **A save format** — `.clayspace`, which already round trips bit-identically.

## What is NOT in scope, and why

The brief describes a mobile application. This library has no UI, no renderer
and no input handling, and the roadmap already records that boundary — VR and
texture painting are out for the same reason. Specifying them here would
produce requirements nothing in this repository can satisfy or test.

Out: touch gestures and the mode bar, camera navigation, compute shaders,
hardware instancing and draw calls, viewport frame rate and battery policy,
Metal/Vulkan minimums, RAM budgets, and a `.zsm` cloud format. Those belong to
the host, and the host already has what it needs to build them: this change
gives it the armature, its edits, and the skinning.

**One item from the brief is dropped on technical grounds rather than scope.**
The node list includes a rotation `q`. A sphere is isotropic, so rotating one
changes no distance and no surface — it is inert in a field. In ZBrush it
matters because the adaptive skin lays out **quads** whose edge flow follows
the node frames; the meshers here are marching cubes, surface nets and dual
contouring, none of which consults such a frame. Storing a value that provably
cannot affect the output would be a promise this engine does not keep. If
per-node orientation is wanted later it needs a reason — a non-circular link
cross-section is the obvious one — and that is a separate change.

## Approach

**The primitive.** A new tape opcode carrying, in the blob, `count` nodes of
`(x, y, z, radius)` and `count` parent indices, plus the blend radius. Node 0
is the root and its parent is itself. Evaluation is the loop above over parent
pairs. A node whose parent is itself and which no child names contributes a
bare sphere, so a one-node armature is a sphere and never degenerate.

**The edits.** The tree is a document concept, so it goes through the command
vocabulary and inherits undo, protection and serialisation: add a child to a
node, move a node, set a radius, delete a node with its subtree. Moving a node
moves its subtree with it — that is the property the whole feature exists for,
and it is a scene-level rule rather than a kernel one.

**Symmetry at insert time.** A mirrored insert adds the node and its reflection
as one undo step, the way `set_mirrored` already does for voxels. The layer
mirror handles the field; this handles the *authoring*, which is where a
sculptor needs it.

## Open questions

- **The order of the smooth union at a branch.** `csmin_quadratic` is not
  associative, so three links meeting at a hip give a slightly different field
  depending on the order they fold in. Deterministic order is required and a
  test must pin it; whether parent-order or radius-order reads better is a
  judgement to make with a render in front of us.
- **Whether a subtree can be reparented**, or only added and deleted. The
  command's inverse has to be exact either way.
- **How a radius interpolates along a link.** The round cone is linear between
  the two radii, which is what the stroke already does. ZBrush's skin is not
  linear, and matching it is a separate question from having the feature.
- **Whether the armature carries per-node colour.** The stroke does not.

## Impact

`sdf-kernels` gains the opcode and must declare its exactness and Lipschitz.
`scene-model` gains the tree and its commands. `file-io` gains the parent array
in the node record, which is a minor bump. `c-abi` and `python-bindings` gain
the surface, an example and tests. Additive throughout: no existing signature
changes, and the stroke keeps working exactly as it does.
