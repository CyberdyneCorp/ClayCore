# python-bindings — the adaptive brush surface is complete

Delta for `add-shared-brush-runtime`.

## ADDED Requirements

### Requirement: The Python adaptive sculptor exposes the whole brush
`DynamicSculptor.stamp` SHALL accept the automask settings, and the adaptive sculptor SHALL accept the cavity and surface-group estimators, so that every factor the fixed sculptor honours is reachable from Python on the adaptive surface too.

Today the adaptive stamp takes no automask argument at all, so the divergence between the representations is not merely unfixed in Python — it is unreachable, and no example or script could have demonstrated it.

#### Scenario: An automasked adaptive stamp is expressible
- **WHEN** a script stamps an adaptive surface with an automask setting
- **THEN** the binding accepts it and the resulting moved count differs from the same stamp without it

### Requirement: A sculptor's scratch cost is readable from Python
Every sculptor exposed to Python SHALL report its scratch arena's capacity, high-water mark and growth count, under one member name shared by all three, mapping to the corresponding C call.

This is what lets an example assert the allocation discipline against the shipped wheel. The allocation gate replaces `operator new` for a test binary and cannot be shipped to a host, so without a reportable arena the claim "a warm stamp allocates nothing" is only ever provable inside the test suite.

#### Scenario: The gallery can assert the discipline
- **WHEN** an example runs a stroke and reads the arena's growth count before and after its warm-up
- **THEN** the count stops rising, and the example fails loudly if it does not
