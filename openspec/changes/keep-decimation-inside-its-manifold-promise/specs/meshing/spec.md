## ADDED Requirements

### Requirement: Decimation does not break a manifold it was given

Decimation SHALL NOT return a mesh carrying an edge with more than two incident
triangles when the mesh it was given carried none.

Simplification is performed by a third-party quadric collapser that applies its
own rules, and not the link condition this codebase refuses a collapse on. A
collapse that merges two vertices sharing a neighbour not opposite the collapsed
edge pinches the surface, and the result is a mesh that looks correct in a render
and is unusable afterwards. The export path documents itself as watertight and
2-manifold, so a pinched result is not one decimation may return.

**The result SHALL be checked rather than assumed.** No simplification setting
makes this impossible, and a setting observed to avoid it on one mesh has been
observed to cause it on another, so the guarantee SHALL come from measuring the
output and not from the options passed in.

Where the requested simplification pinches the surface, decimation SHALL retry —
preferring a different choice of collapses at the requested size over a larger
result, because the requested size is what the caller asked for. Where no retry
yields a manifold result, decimation SHALL return the mesh it was given rather
than a pinched one: a caller that asked for fewer triangles is better served by
more of them than by a surface it cannot use.

A mesh that arrives already carrying such an edge SHALL be simplified and
returned as before. This requirement is about what decimation breaks, not about
what it repairs.

#### Scenario: A manifold input yields a manifold result
- **WHEN** a watertight 2-manifold mesh is decimated at a ratio that pinches the surface
- **THEN** the returned mesh has no edge with more than two incident triangles, and is still watertight

#### Scenario: The result is still decimated
- **WHEN** such a ratio is recovered from
- **THEN** the returned mesh is substantially smaller than the input, rather than the input itself

#### Scenario: An input that was already pinched
- **WHEN** a mesh carrying an edge with more than two incident triangles is decimated
- **THEN** it is simplified and returned, with no claim made about the result's manifoldness
