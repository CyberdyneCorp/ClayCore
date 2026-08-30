# scene-model — an append inside a group

Delta for `append-into-a-group`.

Stated as ADDED rather than as a modification of "An appended document reuses
its compiled prefix", because that requirement is still in flight in
`reuse-the-tape-prefix` and is not yet in the main spec. This one is additive
either way: it says what the reuse must cover, without restating what the reuse
IS.

## ADDED Requirements

### Requirement: An append inside a group reuses the compiled prefix

Appending a node at the tail of a GROUP's child list SHALL reuse the compiled
prefix on the same terms as appending at the tail of a layer's root list, when
every group on the path from that list up to the root is itself in tail
position. Where a shape qualifies, the cost of one append SHALL NOT grow with
the number of items already in the document.

Where it does not qualify — an insert short of the end at any level, a group
that is not last among its siblings, content shared by more than one layer, or
a non-local combine anywhere above the append — the general recompile SHALL
remain, and SHALL remain the default. Widening this fast path SHALL NOT widen
what it claims: a shape that cannot prove its prefix is still a prefix takes
the slow path.

The result of an append SHALL be indistinguishable from a full compile of the
same document, whichever path produced it. This is what makes the fast path a
speed change and nothing else, and it SHALL be checked rather than assumed —
a fast path that silently stops firing reads as correct, so whether it FIRED
SHALL be observable.

Where an artist puts a node SHALL NOT decide whether the engine is fast.
Grouping is a modelling decision with no performance meaning, and until this
requirement a dab placed inside a group cost 90x a dab placed beside it, at a
thousand items on the reference device.

#### Scenario: A stroke inside a group does not grow with the document
- **WHEN** a stroke of equal-length dabs is added into a group, on documents spanning two orders of magnitude in item count
- **THEN** the per-dab cost is flat across that range, as it is for the same stroke at the layer root

#### Scenario: An append into a group equals a full compile
- **WHEN** dabs are appended into a group one at a time
- **THEN** the tape after each append is identical to a full compile of the document at that point

#### Scenario: The fast path is observable
- **WHEN** a stroke is added into a qualifying group
- **THEN** whether the append path fired is readable, so a case can assert it did rather than infer it from a number

#### Scenario: A group that is not last takes the slow path
- **WHEN** a node is appended into a group that has siblings after it
- **THEN** the general recompile runs, because the compiled prefix no longer ends where the append begins

#### Scenario: An insert short of the end still refuses
- **WHEN** a node is inserted one place before the end of a group's child list
- **THEN** the general recompile runs
