# Tasks: correct-the-undo-scope

## 1. The correction

- [x] 1.1 Verify the claim before changing the sentence: `Command` is a variant
      of nineteen alternatives and none edits a voxel grid;
      `include/clay/scene/commands.h` contains the word "voxel" once, in a
      comment about a serialization minor
- [x] 1.2 Rewrite the requirement to describe the vocabulary that exists, and
      to name the boundary — three history mechanisms, one per representation,
      and no single undo step across two of them
- [ ] 1.3 Add the scenario as a test: with undo enabled, a voxel edit leaves
      `undo_depth` unchanged. It pins the boundary so a later change that
      quietly extends undo has to update the spec rather than drift past it

## 2. The guarantees that already hold

Each was verified against the built library before being written, because a
requirement nobody tested is what produced 1.1.

- [x] 2.1 Protection refuses reordering — measured: `move_layer` on a ghosted
      layer gives `layer 1 is ghosted and takes no edits`, on a locked layer
      the same for lock, and clearing the flag makes the identical call succeed
- [x] 2.2 Hidden is not deleted — measured: the field reads 0.5 at the hidden
      sphere's centre, still 0.5 after a save and reload, and −0.5 once shown
- [x] 2.3 The symmetry plane moves with the layer — measured: lumps at x=±0.5
      both read −0.2 with the plane at the origin; translating the layer by +1
      moves the pair to x=+0.5 and +1.5 and x=−0.5 reads +0.8
- [ ] 2.4 Land the three scenarios as tests. 2.1 and 2.3 have no coverage
      today; 2.2 is covered for evaluation but not across a save

## 3. The four requirements NOT added here

From the ROADMAP's "Requirements taken from their bugs". Named so the omission
is a decision rather than an oversight:

- [x] 3.1 *Masks survive resolution changes and representation bridges* —
      already structural: the mask lattice is addressed in WORLD units and
      sampleable at an arbitrary position, so a consumer at any resolution
      reads the same mask. Worth a scenario; belongs in `voxel-engine`, not here
- [x] 3.2 *Import density is decoupled from object scale* — belongs in
      `file-io`, and is not a scene-model requirement
- [x] 3.3 *Presets survive engine versioning* — `brush-engine` already
      requires a versioned preset schema
- [x] 3.4 *Every destructive operation is preview-committed and undoable,
      including hide* — **NOT satisfied, and not fixable by writing it down.**
      Preview exists per-operation (`move_surface_preview`,
      `lattice_gizmo_preview`) rather than as a protocol, and the destructive
      voxel and mesh operations are outside undo entirely per 1.2. Recorded in
      the ROADMAP as a gap
