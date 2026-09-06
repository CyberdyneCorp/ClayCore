## ADDED Requirements

### Requirement: What an intersect's bound costs is reportable

An intersect is bounded by its layer's extent, and computing that extent walks
every visible node. A host SHALL be able to ask how much of that walking a
document has done, because the cost is invisible from outside: the bound is the
same whether it was computed or remembered, so nothing about the result can say,
and a timing on a shared machine says less.

The report SHALL distinguish the walks performed from the ones answered from a
previous edit's memo, since an edit takes the bound on both of its sides and
those two numbers are what say whether the second was saved.

A document whose layers hold no intersect SHALL report nothing at all, because
no other item needs a layer's extent.

#### Scenario: A drag walks its layer once a frame
- **WHEN** an intersecting item is dragged over many frames
- **THEN** the walks performed are about one per frame rather than two, and the remainder are reported as answered from the memo

#### Scenario: A layer with no intersect never walks
- **WHEN** the same drag is made with a subtracting item
- **THEN** no walks and no reuses are reported at all
