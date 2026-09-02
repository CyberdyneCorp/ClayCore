## ADDED Requirements

### Requirement: An appended document reuses its compiled prefix
When a document differs from one already compiled only by nodes appended to the end of the last visible SDF layer's root list, the compiler SHALL be able to produce the new tape by reusing the compiled prefix and compiling only the appended nodes.

The result SHALL be **bit-identical** to a full compile of the same document — every instruction, every parameter, every blob float, the field info and the bounds. Not "within tolerance": a tape that differs anywhere is a different field, and the reuse is only sound because emission is append-ordered and nothing already emitted moves.

Reuse SHALL be attempted only where that append-ordering holds. A node inserted anywhere but the tail, a node removed or moved, any edit to an already-compiled node, and an append to any layer but the last visible SDF layer all SHALL compile in full. Refusing to reuse costs a recompile, which is the behaviour that existed before; reusing when the prefix has in fact moved is silent and wrong, so where it is not obvious the compiler SHALL compile in full.

A compiled tape SHALL remain immutable, and its content identity SHALL keep meaning what it means today: a tape built from a reused prefix has different bytes from the tape whose prefix it borrowed, and therefore SHALL carry its own distinct identity rather than inheriting one.

#### Scenario: A reused prefix gives the same tape as a full compile
- **WHEN** a document is compiled, one or more nodes are appended to the end of the last visible SDF layer, and the tape is compiled again with prefix reuse
- **THEN** the resulting tape is bit-identical to compiling the appended document from scratch

#### Scenario: Reuse holds across what the compiler emits per item
- **WHEN** the appended-to layer carries a mirror, a radial symmetry, a mask or a layer transform, and when the appended nodes carry blends, groups, strokes, sampled volumes, gates or deformer chains
- **THEN** the reused result stays bit-identical to a full compile in every case

#### Scenario: An edit that is not a tail append compiles in full
- **WHEN** a node is inserted before the end of the list, removed, moved, or edited in place, or a node is appended to a layer that is not the last visible SDF layer
- **THEN** the tape is compiled in full and matches a fresh compile of the edited document

#### Scenario: A reused tape carries its own identity
- **WHEN** a tape is produced by reusing another tape's prefix
- **THEN** its content identity is nonzero and differs from that of the tape whose prefix it reused, and the tape it reused from is unchanged
