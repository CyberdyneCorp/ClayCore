## MODIFIED Requirements

### Requirement: Detail is stored in a transported local frame
Detail SHALL be stored as coefficients in a local frame — tangent, bitangent and normal — derived from the subdivided parent surface, and SHALL NOT be stored only as a world-space offset.

A world-space offset is adequate for small changes and fails at the case the feature exists for: when the parent surface rotates or bends, a stored world vector no longer points along the surface it belongs to, and detail shears away from the form that carried it.

The frame SHALL be TRANSPORTED rather than rebuilt from whichever neighbour is encountered first: a UV tangent where a valid parametrization exists, a deterministic geometric tangent otherwise, rotated by the shortest arc when the parent normal moves, with sign consistency enforced against the previous frame. An unstable frame rotates detail, and the artefact appears in a render rather than in a numeric test.

A vertex's frame at a region boundary DOES still depend on which patches are
resident at its level, and this change does not remove that. The frame is built
by rotating the parent frame's tangent onto the child's own level normal, and
that normal is an unweighted sum over the faces incident to the vertex — so
where a level stores only part of the surface the sum is one-sided, and the
frame a coefficient is stored in differs from the one a uniformly refined
hierarchy would have built. Measured on a 6x6 cage with the middle 2x2 refined
to level 3, walking the resident patches face by face: 124 of 1024 emitted
corners carry a different frame, worst |Δnormal| 0.170116 at level 2 and
0.154028 at level 3 — about 10 and 9 degrees — and the DISPLAY normal, which is
the same one-sided sum, differs at the same corners by the same amounts.

The consequence is a STORAGE one rather than a shading one, and it is recorded
here as a LIMIT rather than left for a reader to discover. Because a level's
position is its subdivided parent plus the frame applied to the detail, a frame
that differs means the same authored coefficient reconstructs to a different
world offset: with identical `LocalDetail` written into every level-3 vertex of
both hierarchies, 118 of 1024 shared corners land somewhere else, worst 0.0057
on a cage two units across. So the guarantee that a regional level holds a
uniform hierarchy's numbers is unconditional only while the detail at a boundary
vertex is zero, which is the case the shipped bit-identity gate exercises.

WHAT IS COMPLETE AT A BOUNDARY IS THE BRUSH'S READING OF THE SURFACE, which is a
different set of call sites and is required as such by the neighbourhood
requirement below: the angle-weighted per-vertex normal every displacing verb
steers by, the normal recompute a stamp leaves behind, the averaged ring a
smoothing verb divides by, and the border predicate boundary automasking fires
on all take the faces a coarser level holds. The frame and the display normal
come from the OTHER of the two normal evaluators — the unweighted sum over a
level's own faces — which stays a second evaluator by the decision recorded
below, and which has not been given the complete face set. Giving it one also
widens the propagation halo that decides which vertices are re-derived at all,
and makes averaging coefficients across a transition meaningful for the first
time, so the three land together or not at all.

Detail SHALL be authoritative in single precision. It SHALL NOT be quantized in this change: high-frequency detail is where a visible artefact appears first, and any compression waits on a measured error bound.

#### Scenario: A wrinkle survives a bend
- **WHEN** detail is sculpted at a fine level and the parent surface is then bent at a coarse one
- **THEN** the detail remains attached to the surface, in the same local orientation, rather than shearing away from it

#### Scenario: A small deformation does not flip the frame
- **WHEN** a parent surface is deformed slightly and the frames are rebuilt
- **THEN** no frame reverses sign, and the reconstructed detail does not rotate

#### Scenario: A region boundary reconstructs densely while its detail is zero
- **WHEN** a regionally refined hierarchy and a uniformly refined hierarchy over the same cage are evaluated with no detail authored at the boundary
- **THEN** every vertex the two share holds the same position, bit for bit
- **WHEN** the same non-zero detail coefficients are then written into the boundary vertices of both
- **THEN** the two reconstruct to different positions, because the frames those coefficients are measured in differ, and that is the limit stated above rather than a defect a caller can work around

## ADDED Requirements

### Requirement: A vertex's neighbourhood is complete across a depth boundary

Where a level stores only part of the surface, a vertex on the edge of what it
stores SHALL still be given its COMPLETE surface neighbourhood — the faces,
corners and vertices that a uniformly refined hierarchy would have given it,
including those that live at a coarser level. A regional level's own connectivity
ends at the region rim, and every walk built on it therefore sees an open border
where the surface in fact continues.

The failure this forbids is not a wrong neighbour but a MISSING one, and it is
silent: no site refuses, clamps or misdirects, because a coarse neighbour has no
vertex, no weld class and no face at the fine level to be picked wrongly. The
damage is truncation and one-sidedness — an averaged position biased into the
refined region, a normal tipped toward it, a flood fill that stops.

The neighbourhood SHALL be provided as ONE topology answer that names each
incident face by the level it lives at, and SHALL NOT be reimplemented per
algorithm. It SHALL NOT, however, be a single callback carrying every payload:
the readers of a neighbourhood want nine different things — a position at the
evaluated surface, a position at the subdivided surface, detail coefficients, a
geometric normal, a colour, a workset slot, a triangle with its corner and
interior angle, an incident face, and a shared-face count — and a smoothing verb
re-reads its neighbourhood once per pass, so a materialized answer read many
times is required and a per-neighbour callback is not. The two normal evaluators
that exist over a level — one angle-weighted over triangles, one an unweighted
sum over faces — SHALL remain two; merging them would change one of their results.

The order in which a neighbourhood is reported SHALL be deterministic and stated,
because float addition is not associative and the sums taken over a neighbourhood
are part of the result.

An identity that names a vertex at another level SHALL carry the same staleness
discipline as the existing seed token: a level change or a cache generation
change renumbers a level's vertices, and a regional level's numbering is
compacted to what it stores.

**A depth transition SHALL NOT be reported as a border of the model.** Automasking
that protects the open edge of an unclosed mesh SHALL NOT fire at an internal
seam between two resident levels, because the artist cannot see that seam and did
not put it there.

#### Scenario: A boundary vertex has the neighbourhood a uniform hierarchy gives it
- **WHEN** a vertex on the boundary of a refined region is asked for its incident faces
- **THEN** the set is the one a uniformly refined hierarchy would report for the same vertex, counting the faces that live at the coarser level

#### Scenario: The normal a brush steers by is complete at a boundary
- **WHEN** a vertex on the boundary of a refined region is asked for the angle-weighted normal a displacing verb steers by, or has its normal recomputed after a stamp
- **THEN** the sum is taken over its complete incident face set, including the faces that live at the coarser level
- **WHEN** the same vertex's DISPLAY normal or transported frame is read instead
- **THEN** it is still summed over the level's own faces alone, which is the limit the frame requirement records

#### Scenario: A smoothing verb is not dragged inward at a seam
- **WHEN** a smoothing stroke is applied across the boundary of a refined region, and the same stroke is applied to a uniformly refined hierarchy
- **THEN** the vertices the two hierarchies share finish in the same place, rather than the boundary being pulled into the refined region by a one-sided average

#### Scenario: Boundary automasking ignores an internal seam
- **WHEN** boundary automasking is enabled and a stroke crosses a depth transition
- **THEN** no vertex is faded for being at the transition, while a vertex on the model's own open edge is still faded

#### Scenario: A neighbourhood is reported in a stated order
- **WHEN** the same neighbourhood is requested twice, on any platform
- **THEN** the neighbours come back in the same order, and any sum taken over them is bit-identical

### Requirement: A brush stroke crosses a depth boundary

A stamp whose footprint reaches past the region a level refines SHALL write the
part of that footprint that lies on the coarse side, at the level that side
actually lives at, and SHALL NOT deposit its full falloff at the region rim and
nothing beyond it.

The surface an artist is looking at on a regionally refined hierarchy is not all
at one level: the patches they refined are at the sculpt level and the patches
beside them are one or more levels coarser, with no vertex at the sculpt level
for a brush to move. A stamp confined to the sculpt level therefore stops dead at
the rim with the falloff still near full — a STEP in the displacement rather than
a fade — and reports the same count it would have reported had it done its whole
job. Measured on a small refined region with a stamp anchored on its rim, up to
46 vertices of the emitted surface are moved by the same stamp on a uniformly
refined hierarchy and by nothing at all here, and the surface finishes up to 4.4
times the fine level's edge spacing from where the uniform hierarchy leaves it.

WHICH LEVEL A VERTEX IS WRITTEN AT SHALL be the level the mixed-depth surface
carries it at, and SHALL NOT be a second rule: a vertex belongs to its own level
unless the level above holds its vertex point, in which case the finer vertex is
the one the artist is looking at and the one the brush moves. Every vertex of that
surface therefore belongs to exactly ONE level, which is what makes a DOUBLED
contribution at the seam impossible by construction rather than something a
tolerance has to catch.

The levels SHALL be written COARSEST FIRST, and the finer levels SHALL be written
as absolute positions rather than as displacements accumulated before them. A
coarse write moves the subdivided surface underneath the finer levels beside it,
so a finer level whose coefficient was stored BEFORE that write reconstructs to
the position the brush asked for plus that ripple. Measured, that ordering error
is 21 times the residual of the correct order.

What remains after that SHALL be understood as the coarse level's own resolution
and not as a seam: a level cannot represent a displacement finer than its own
spacing, so a stroke crossing a boundary finishes near, not at, the answer a
uniformly refined hierarchy gives — measured within a quarter of the fine level's
edge spacing for every displacement verb.

A hierarchy whose levels all refine every patch owns nothing below its top level
and SHALL take none of this: its stamps SHALL be byte-identical to what they were
before a crossing stamp existed.

#### Scenario: A stamp reaching past a refined region moves the coarse side
- **WHEN** a stamp is anchored on the rim of a refined region with a radius that reaches past it
- **THEN** every vertex of the emitted surface that the same stamp moves on a uniformly refined hierarchy is moved here too, rather than a subset of them

#### Scenario: No vertex is written twice
- **WHEN** a stamp writes both a coarse level and the level above it
- **THEN** no vertex appears in both write lists, and no displacement is applied to the seam twice

#### Scenario: The coarse side is written before the fine one
- **WHEN** a stamp crosses a depth boundary
- **THEN** the surface finishes where it finishes on a uniformly refined hierarchy, rather than at that position plus the coarse write's own effect on the level above

#### Scenario: A crossing stamp leaves the surface watertight
- **WHEN** a stamp that moved both sides of a depth boundary is followed by a mixed-depth export
- **THEN** the export still has no open edge, because the shared vertex the coarse face borrows is the one the stamp moved

#### Scenario: A crossing gesture is one undo step
- **WHEN** a stamp that wrote two levels is recorded and then reverted
- **THEN** both levels come back byte for byte, and the record names both of them

#### Scenario: A hierarchy of one depth is unaffected
- **WHEN** a stamp is made on a hierarchy whose levels all refine every patch, or inside a refined region and nowhere near its rim
- **THEN** exactly one level is written, and the result is byte-identical to a stamp made on that level's own mesh and absorbed into it

### Requirement: A mixed-depth hierarchy exports as one watertight mesh

A hierarchy whose patches carry different depths SHALL be exportable as ONE mesh
that is watertight BY CONSTRUCTION. Today a caller that follows the documented
per-patch path — copying each patch's block at that patch's effective level —
gets a mesh with open edges at every depth boundary, and a caller that exports a
single level gets only the patches resident at it.

Watertightness SHALL be achieved by IDENTITY and never by tolerance welding. A
vertex shared between a coarse face and a finer patch SHALL be one vertex in the
output, carrying the value the finer side computed, because that value is one
subdivision step closer to the limit. The two sides are a SUBDIVISION STEP apart
rather than a hairline apart — measured at 8.7% of a cage edge — so any epsilon
large enough to close the gap would weld unrelated geometry, and the hierarchy's
own adjacency already welds at exactly zero for the same reason.

Adopting the finer value at a shared vertex moves the coarse face's corner, so
EVERY coarse face incident to that vertex SHALL adopt it — including a face whose
patch meets the finer region only at a CORNER and therefore has no split edge and
no T-junction. Emitting only for faces that share an EDGE with a finer patch does
not remove the crack; it relocates it one face over onto a boundary between two
coarse faces, where it presents as an unrelated defect.

The export SHALL keep the mesh's QUAD LIST wherever the transition allows it, and
SHALL NOT keep a quad list that does not describe its indices. Those two are in
tension at a split edge and the arithmetic decides between them: a coarse quad
with one split edge is a pentagon, and a polygon with an ODD number of boundary
vertices has no quadrangulation at all — four edges per quad counts every
interior edge twice, so the boundary count must be even however many vertices are
added inside. Making it even would mean splitting a second edge of that face,
whose new vertex the neighbouring coarse face must then also carry, and so on out
of the transition and across the model.

So the guarantee is stated as a boundary rather than as an absolute: an export
whose transitions are all CORNER-ONLY — where the finer region is met at a cage
vertex and no coarse edge is split — SHALL be emitted as quads with the quad list
intact, as SHALL every uniform-depth export; an export containing a split edge
SHALL be emitted as a triangle list with no quad list, because a quad list
describing indices that no longer exist is a lie a saved document would carry.

WHICH CAGES REACH THE CORNER-ONLY CASE, so the middle of that boundary is a
gated branch rather than a sentence no fixture can stand on. A coarse patch
sharing an EDGE with a refined one has that edge split, so a whole export with no
split edge anywhere requires the refined set to be closed under edge adjacency —
which on an edge-connected cage means every patch or none, and that is the
uniform case. The corner-only case is therefore reached only where two parts of
the cage meet at a VERTEX without sharing an edge, and it is gated on exactly
that: two quad grids joined at one cage vertex, one half refined, whose export
keeps its quad list and whose coarse face at the join still emits two triangles
for its one face. Per-patch, the case is commoner than that — a corner-only
patch beside an ordinary refined block is one that shares only a cage vertex
with it — and the per-patch counts are gated too.

The output SHALL be DETERMINISTIC — the same hierarchy emits the same faces in the
same order on every run and on every platform — and STABLE under re-refinement:
refining an unrelated region SHALL leave the emitted faces of a coarse face whose
neighbourhood residency did not change byte-identical.

Transition geometry SHALL be DERIVED and SHALL NOT be serialized. It is a
function of the cage, the subdivision rule and the per-level patch sets, all of
which a stored surface already carries, so adding it SHALL NOT change the
serialization version and SHALL NOT make an older reader refuse a document. It
SHALL NOT become the authoritative sculpt representation.

An export that cannot present the whole surface SHALL SAY SO rather than return a
partial result reported as success.

#### Scenario: A mixed-depth export has no open edges
- **WHEN** a hierarchy with different depths in different regions is exported as one mesh
- **THEN** the result has no boundary edge that the base cage did not have, and no vertex is welded by proximity

#### Scenario: A coarse patch touching a finer one only at a corner is emitted too
- **WHEN** a coarse patch shares only a cage vertex, and no edge, with a finer patch
- **THEN** it is emitted with the finer side's value at that corner, and no crack appears on its boundary with the coarse patches beside it

#### Scenario: The quad list survives exactly as far as the split edges allow
- **WHEN** a hierarchy whose depths all agree, or whose transitions are all corner-only, is exported
- **THEN** every emitted face is a quad and the mesh's quad list describes its indices
- **WHEN** the export contains a coarse face with a split edge
- **THEN** that face is emitted as a pentagon and the mesh carries no quad list at all, rather than a quad list that describes triangles it does not have

#### Scenario: Refining elsewhere does not disturb a transition
- **WHEN** a region of a hierarchy is refined and an unrelated coarse face's neighbourhood residency is unchanged
- **THEN** that face emits the same transition faces, byte for byte, as it did before

#### Scenario: A mixed-depth export survives a round trip without a format change
- **WHEN** a mixed-depth hierarchy is saved, reloaded and exported
- **THEN** the export is identical, and a build without the transition feature still reads the document
