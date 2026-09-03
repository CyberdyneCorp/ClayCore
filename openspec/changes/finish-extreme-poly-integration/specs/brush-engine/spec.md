## MODIFIED Requirements

### Requirement: A stroke does not re-find an anchor it already has

A geodesic region is grown from an ANCHOR class, and finding that anchor without
a spatial index costs a scan over every class in the surface. A sculptor SHALL
NOT pay that scan once per dab: after a stamp has resolved an anchor, the next
stamp of the same stroke SHALL start from it rather than searching the model
again. This is what makes a stamp on a subdivision level cost what it touches —
the hierarchy builds no ray tree, so without it every dab is proportional to the
level's vertex count.

**A carried anchor SHALL NOT change what a stamp produces.** The walk accumulates
its path distance from the anchor, so the anchor is an input to the falloff and
not merely a starting point for a search: substituting a different class would
move the surface. A carried anchor SHALL therefore be used to LOCATE the class
the unaccelerated search would have returned, and the positions, colours and
moved counts a stroke produces SHALL be identical to those it produced when every
dab searched from scratch.

A carried anchor SHALL be validated before use and discarded rather than trusted:
one from a retired class space, or one further from the new centre than the brush
radius, SHALL fall back to the unaccelerated search. An anchor further than the
radius makes the walk return an EMPTY region, so an unvalidated one does not
misplace a dab, it silently loses it — indistinguishable to a host from a fully
masked stroke.

#### Scenario: A stroke's dabs do not each search the model
- **WHEN** a stroke of many dabs is applied to a surface with no spatial index, at a footprint far smaller than the surface
- **THEN** the classes examined to anchor each dab after the first follow the footprint rather than the surface's size

#### Scenario: An accelerated stroke and a cold one agree exactly
- **WHEN** the same stroke is applied twice, once where every dab searches from scratch and once where each dab starts from the previous dab's anchor
- **THEN** the two surfaces are identical — positions, colours and moved counts — rather than close

#### Scenario: An anchor out of reach is discarded, not spent
- **WHEN** a dab lands further from the carried anchor than the brush radius
- **THEN** the anchor is discarded and the dab moves the vertices it would have moved with no anchor at all, rather than moving nothing
