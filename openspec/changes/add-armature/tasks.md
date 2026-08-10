# Tasks: add-armature

- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): touches the tape, the scene model and the node record, so it does NOT run beside another change that edits `ctape_*` or `write_node`/`read_node`. It is independent of the VoxelGrid work.
- [ ] 0.2 This change takes `.clayspace` minor **9**. 5, 6, 7 and 8 are already assigned (mesh layers, multi-resolution, sculpt layers, round trip). Bump `kClaySpaceMinor` and `kSceneMinor` together — a static_assert binds them.

- [ ] 1.1 DECIDE and record the fold order of the smooth union at a branch, since `csmin_quadratic` is not associative. Deterministic, and pinned by a test whatever it is
- [ ] 1.2 `ctape_armature`: nodes as (x, y, z, radius) plus a parent index array in the blob, evaluated over `(i, parent[i])`. Reuses the stroke's segment maths — round cone, capsule when the radii match, sphere when the endpoints coincide
- [ ] 1.3 A chain-shaped armature evaluates IDENTICALLY to the stroke with the same points. This is the compatibility test that keeps the generalisation honest
- [ ] 1.4 Exactness and Lipschitz: a smooth union is a bound, and the declaration must follow what the stroke already declares rather than being invented
- [ ] 1.5 Bounds and influence: an armature's bound is the union of its node spheres dilated by the blend, so culling keeps working
- [ ] 1.6 Scene: the tree on a Node, with add-child / move / set-radius / delete-subtree as commands with exact inverses. Moving a node moves its subtree
- [ ] 1.7 Mirrored insert as ONE undo step, following `set_mirrored`'s precedent
- [ ] 1.8 `.clayspace` at minor 9, backward-open: an older reader that meets an armature skips it rather than failing
- [ ] 1.9 C ABI (additive) and pyclay, binding parity clean
- [ ] 1.10 Tests: a chain armature equals the stroke; moving a shoulder carries the arm; deleting a node takes its subtree and undoes exactly; a one-node armature is a sphere; a branch is deterministic across backends; round trip is bit-identical; parity across every registered backend
- [ ] 1.11 Example `examples/NN_armature.py` (take the next free number; 39 is the highest today): block a figure out as an armature, move one node to show the subtree follow, mirror a limb, then skin it with `Document.mesh`. It replaces hand-written coordinates with a tree, which is the whole point, and should say so against `34_organic_character.py`
- [ ] 1.12 Roadmap: record the minor, and record that rotation is deliberately absent because it cannot affect an isotropic field
