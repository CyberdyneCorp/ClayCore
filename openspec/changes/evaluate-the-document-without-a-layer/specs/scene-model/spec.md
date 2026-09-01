# scene-model

## ADDED Requirements

### Requirement: A document part may exclude one layer

The document-part compile SHALL offer a third selection beside "the visible SDF
layers before the active one" and "only the active one": **every visible SDF
layer except the named one**, joined by the same hard union a whole-document
compile emits between layers.

The excluded layer SHALL be excluded wherever it sits in the stack, not only
when it is last. A selection that stopped at the named layer would be the
existing "before" case under another name, and would silently drop every layer
above it.

The excluded part SHALL cull under the WHOLE DOCUMENT's pad, on the same terms
and for the same reason the other two parts do: a part compiled under its own
smaller pad drops items the whole-document compile keeps, and the parts then no
longer sum to the whole.

Excluding a layer that is hidden, that is not an SDF layer, or that the document
does not hold SHALL be an ordinary compile of every visible SDF layer — the
compile itself has nothing to refuse, because such a layer contributes nothing
to the union in the first place. Refusing a caller who names one is the C ABI's
to do, where the caller's intent is known.

#### Scenario: The excluded layer is in the middle of the stack
- **WHEN** a document with three visible SDF layers is compiled excluding the middle one
- **THEN** the tape evaluates to the hard union of the first and third, and the middle layer's items appear in it nowhere

#### Scenario: The parts sum to the whole
- **WHEN** a document is compiled excluding one layer, and that layer is compiled on its own
- **THEN** the hard union of the two values equals what a whole-document compile evaluates to at the same points

#### Scenario: An excluded part culls under the document's pad
- **WHEN** a part excluding one layer is compiled under a cull region
- **THEN** it keeps every item a whole-document compile under that region keeps, so the two parts still sum to the whole

#### Scenario: Excluding the only visible layer leaves empty space
- **WHEN** a document whose one visible SDF layer is the excluded one is compiled
- **THEN** the compile produces a tape that evaluates as empty space rather than failing
