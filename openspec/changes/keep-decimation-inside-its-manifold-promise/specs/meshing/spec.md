## ADDED Requirements

### Requirement: Decimation says when it has pinched a surface

Decimation SHALL report whether its result carries an edge with more than two
incident triangles, and whether the mesh it was given did.

Simplification is performed by a third-party quadric collapser that applies its
own rules, and not the link condition this codebase refuses a collapse on. A
collapse that merges two vertices sharing a neighbour not opposite the collapsed
edge pinches the surface, and the result is a mesh that looks correct in a render
and is unusable afterwards — the defect that breaks a slicer or a boolean engine
while a viewport shows nothing wrong. A caller cannot act on that without being
told it happened.

**Where a pinch is incidental, decimation SHALL try to avoid it**, by asking the
collapser for a different choice of collapses at the same target, and SHALL
prefer a clean result.

**Decimation SHALL NOT grow a result to obtain one.** A caller chooses a ratio
because it needs that size, and a larger mesh is a different problem rather than
a solution to this one — so a clean retry that is materially larger than the
result the caller would otherwise have received SHALL be rejected in favour of
reporting the pinch. This applies to a retry that overshoots the target as much
as to one that asks for more: holding the target does not by itself hold the
size.

**Where no choice of collapses is clean, the requested size SHALL be returned
and reported as pinched.** At an aggressive ratio a pinch is not an incidental
bad collapse — merging sheets is what the ratio means — so refusing such a
result would mean refusing to decimate, and returning the undecimated input
instead serves no caller who asked for a fraction of the geometry.

A mesh that arrives already carrying such an edge SHALL be simplified and
returned, and reported as having arrived that way. This requirement is about
what decimation breaks, not about what it repairs.

#### Scenario: An incidental pinch is avoided
- **WHEN** a watertight 2-manifold mesh is decimated at a ratio whose first simplification pinches the surface, and another choice of collapses at the same target does not
- **THEN** the clean result is returned, and it is reported as manifold

#### Scenario: A clean retry that is larger is not taken
- **WHEN** the only clean result is materially larger than the result the caller would otherwise have received
- **THEN** the smaller result is returned and reported as pinched, rather than the larger one

#### Scenario: An unrecoverable pinch keeps the requested size
- **WHEN** no choice of collapses at the requested target is clean
- **THEN** a result of the requested size is returned, and it is reported as not manifold

#### Scenario: An input that was already pinched
- **WHEN** a mesh carrying an edge with more than two incident triangles is decimated
- **THEN** it is simplified and returned, and the report says the input was not manifold
