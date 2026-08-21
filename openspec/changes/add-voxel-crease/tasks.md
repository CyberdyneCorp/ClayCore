# Tasks

## 1. Decide the shape

- [ ] 1.1 CONFIRM the premise before writing the verb: build the same stroke as
      (a) `sculpt_crease` and (b) `sculpt_inflate(-1)` then `sculpt_pinch` as
      two calls, and check the results DIFFER. If they do not, the verb has no
      reason to exist and the answer is a documented recipe instead. This is
      the whole justification, so it is the first task rather than a later test
- [ ] 1.2 DECIDE how far the pinch steps. `radial_step` moves a surface cell
      ONE cell toward the centre, which is what `sculpt_pinch` does; a crease
      may want the step gated on the cut having happened, so the squeeze
      closes the fold rather than eroding the rim independently
- [ ] 1.3 DECIDE what `depth` counts in. Cells is what `sculpt_inflate`'s
      `amount` counts in and is honest about a lattice; world units would read
      better to a host and lie about sub-cell depth. Prefer cells, and let the
      C ABI document the conversion

## 2. The verb

- [ ] 2.1 `VoxelGrid::sculpt_crease(cell, params, normal, depth)`
- [ ] 2.2 ONE snapshot, both decisions read from it — the `sculpt_scrape`
      discipline, for the reason `verb_crease` states on the mesh side
- [ ] 2.3 The cut: erode along `normal`, weighted by the falloff `brush_pass`
      already applies
- [ ] 2.4 The squeeze: surface cells step toward the stamp centre, reusing
      `radial_step` rather than a second copy of it
- [ ] 2.5 `depth < 0` raises a ridge instead of cutting — the Alt behaviour,
      following `sculpt_inflate`'s signed `amount`
- [ ] 2.6 A zero depth writes nothing at all: no cells touched, and nothing
      recorded into an open sculpt layer
- [ ] 2.7 A degenerate normal is refused rather than defaulted, as
      `sculpt_scrape` refuses one

## 3. Pin the properties

- [ ] 3.1 It cuts: a stroke across a flat slab leaves a groove, and the
      groove's floor is `depth` cells below the surface
- [ ] 3.2 It pinches: the groove is NARROWER at the top than an erode-only cut
      of the same depth. That difference IS the verb, so measure it rather than
      eyeball a render
- [ ] 3.3 A negative depth raises a ridge, and the ridge is the cut's mirror
- [ ] 3.4 **Different from the two-call sequence** — 1.1 promoted to a
      regression test, so a later refactor cannot quietly collapse it into
      carve-then-pinch
- [ ] 3.5 A mask holds cells: fully masked cells are untouched, by the one rule
      `brush_pass` already applies to every verb
- [ ] 3.6 An open sculpt layer records it, and dialling that layer to 0 restores
      the surface — a crease is a pass worth regretting, which is what layers
      are for
- [ ] 3.7 Deterministic, including the dithered fractional-strength case: the
      same seed and strength pick the same cells on every run and platform
- [ ] 3.8 Order independence: the result does not depend on the order cells are
      visited, which is what one snapshot buys and what the parallel split
      above `kParallelBrushSpan` would otherwise break

## 4. Reach it

- [ ] 4.1 C ABI: `clay_voxel_sculpt_crease`
- [ ] 4.2 pyclay, so `check_binding_parity` stays clean
- [ ] 4.3 The device coverage gate: `clay_voxel_sculpt_*` is already matched by
      `VERB_PATTERNS`, so this verb WILL be discovered and needs either a case
      or a recorded exemption. Check which rather than letting the gate decide
      on the iPad
- [ ] 4.4 ABI 0.39.0 in all three places the release checklist names

## 5. Say what it does, and what it cannot

- [ ] 5.1 Spec delta on `voxel-engine`
- [ ] 5.2 `docs/07-brushes-and-features.md`: the voxel verb table, and the
      DamStandard row in the ZBrush mapping, which currently points only at
      `Op::Incise`
- [ ] 5.3 `docs/sculpt_comparison.md`: the DamStandard row becomes all three
      representations
- [ ] 5.4 The README's voxel verb list says "10 sculpting verbs"
- [ ] 5.5 An example, and it must show the LIMIT as well as the verb: the same
      crease at a coarse cell size and at a refined level, so the staircase is
      visible and the resolution stack is visibly the answer
