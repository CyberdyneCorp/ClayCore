# Tasks: SUPERSEDED — see proposal.md

Deliberately left unticked. `voxel-to-field` delivered this capability, and
ticking these would claim its work for a plan that did not do it. Task 1.1 —
"conversion in place, or a new layer beside the original" — was answered as a
new layer beside the original, which is why the requirement in
`specs/scene-model/` describes a design this library does not implement.


- [ ] 1.1 DECIDE: conversion in place, or a new layer beside the original, and record why
- [ ] 1.2 Voxel to narrow-band signed distance directly, without the mesh detour, with a real Lipschitz bound rather than the step function
- [ ] 1.3 Carry the palette across, and state what it becomes
- [ ] 1.4 Write down the guarantees the existing SDF-to-voxel direction already provides
- [ ] 1.5 State the round-trip tolerance, and choose whether it is a distance or a volume-difference bound
- [ ] 1.6 Both bindings, C ABI additive
- [ ] 1.7 Tests: a shape survives a round trip within the stated tolerance; colour survives; a boolean's edge degrades to the cell size and the test asserts THAT rather than pretending it does not; step scale reported honestly after conversion
- [ ] 1.8 Example: block out with booleans, convert, sculpt with the voxel verbs, convert back, boolean again
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): waits for add-multi-resolution; needs a `.clayspace` minor (8) only if it introduces a new layer kind, which is an open question
