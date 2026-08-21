# Tasks

## 1. Decide the shape

- [ ] 1.1 DECIDE where it lives. `mesh/` is the obvious home, but the only
      caller today is `voxel/grid.cpp`, and voxel may include mesh while mesh
      may not include voxel — so `mesh/` works and the reverse would not.
      Confirm against `tools/check_layering.py` rather than assuming
- [ ] 1.2 DECIDE the fallback when no source attribute exists: refuse the call,
      or leave the target's own attribute alone? Leaving it alone composes
      better (transfer colour from A, uvs from B) but makes "nothing happened"
      quiet. Whichever, the count reported has to make it visible
- [ ] 1.3 DECIDE whether the distance threshold is absolute or relative to the
      source's bounds. Absolute is predictable; relative survives a host that
      works in millimetres. Probably relative with an absolute override

## 2. The operation

- [ ] 2.1 `transfer_attributes(source, target*, options)` over the target's
      vertices, reading the source through one `Bvh` built once
- [ ] 2.2 Colours and uvs by default; normals opt-in, and the option's
      documentation says why the default is off
- [ ] 2.3 Positions never move — asserted, not merely intended
- [ ] 2.4 A per-vertex distance threshold, with a documented fallback past it
- [ ] 2.5 Report what happened: how many vertices were transferred, and how
      many took the fallback. A result that silently fell back for most of the
      mesh must be distinguishable from a good one
- [ ] 2.6 `mesh_colour_at` in `src/voxel/grid.cpp` becomes a caller rather than
      a private duplicate — the point of generalising it

## 3. Pin the properties

- [ ] 3.1 An identity transfer is exact: transferring a mesh's attributes onto
      ITSELF returns them bit-identically, which is the only case where an
      exact answer is available and therefore the only honest exactness test
- [ ] 3.2 Positions and topology are byte-identical before and after
- [ ] 3.3 A target vertex beyond the threshold takes the fallback, and the
      report counts it
- [ ] 3.4 A source with no colours (or no uvs) does whatever 1.2 decided, and
      the test says which
- [ ] 3.5 The round trip it exists for, end to end: a coloured, UV'd mesh →
      `Volume.from_mesh` → mesh → transfer from the original. Colour comes back
      close; measure HOW close rather than asserting it does
- [ ] 3.6 The uv seam case, as a test that DOCUMENTS the limitation rather than
      one that fails: a source with a seam, a target vertex on it, and an
      assertion about what actually happens — so the behaviour is pinned and
      the next reader does not think it is a bug
- [ ] 3.7 Deterministic

## 4. Reach it

- [ ] 4.1 C ABI
- [ ] 4.2 pyclay, so `check_binding_parity` stays clean
- [ ] 4.3 ABI minor bump in all three places the release checklist names

## 5. Say what it does and does not do

- [ ] 5.1 Spec delta on `meshing`
- [ ] 5.2 `docs/08-mesh-readback.md` or `docs/07`, whichever owns "getting a
      mesh out and back", gains the round-trip recipe
- [ ] 5.3 The README's composability row currently says a mesh conversion
      "drops the UVs and edge loops". After this it drops the edge loops and
      MOSTLY keeps the uvs, which is a different sentence
- [ ] 5.4 An example measuring what comes back and what does not — the colour
      recovery and the topology that is still gone
