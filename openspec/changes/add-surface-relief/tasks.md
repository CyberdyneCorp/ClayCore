# Tasks: add-surface-relief

- [ ] 1.1 `ccombine_relief`: offset the accumulator by a signed amplitude, weighted
      by the item's own field as a region, following the paint op's precedent
- [ ] 1.2 Finite support, so influence bounds and brick culling are unaffected
- [ ] 1.3 `cfi_relief`: amplitude over falloff width
- [ ] 1.4 Bounds: the surface can move by the amplitude, so the bound grows by it
- [ ] 1.5 Scene op, C ABI and Python bindings, on the existing parameter convention
- [ ] 1.6 A parity corpus row
- [ ] 1.7 Tests: it builds up and cuts in, it contributes nothing alone, support is
      finite, zero is a no-op, the declared Lipschitz holds, a ray still lands,
      and it survives a round trip
- [ ] 1.8 Docs, example, full verification
