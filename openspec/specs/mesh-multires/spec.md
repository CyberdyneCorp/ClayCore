# mesh-multires Specification

## Purpose
Coarse form and fine detail on one surface, each editable without destroying the
other.

A level is its parent subdivided plus its own detail, and the detail is stored
in a frame transported with the surface — which is what lets a sweep at a low
level carry the pores at a high one instead of smearing them. The sculpt level
is independent of the display level, propagation between them is local, adding a
level reports its cost before paying it, and a hierarchy carrying detail refuses
the arbitrary topology change that would leave that detail describing a surface
that no longer exists.

Its own capability rather than part of dynamic-topology because the two are
opposite bargains: that one changes the triangle count to follow the brush, this
one fixes a base cage and layers detail over it.

## Requirements

### Requirement: A level is its parent subdivided plus its own detail
The library SHALL represent a multiresolution surface as a base mesh, a deterministic subdivision hierarchy, and per-level detail, where a level's positions are its subdivided parent's positions PLUS that level's detail.

Levels SHALL NOT be stored as unrelated absolute meshes. Without the relationship a change to a lower level has no defined effect on a higher one, so either the fine detail or the coarse edit must be discarded — and preserving both is the whole purpose of the hierarchy.

Level generation SHALL be DETERMINISTIC: the same base mesh and the same rule SHALL produce the same hierarchy on every platform and every run.

A level SHALL be exportable as an ordinary `mesh::Mesh`, so that every existing consumer — meshers, exporters, validation, the readback accessors — sees a flat mesh and needs no knowledge of the hierarchy.

#### Scenario: A level exports as an ordinary mesh
- **WHEN** any level of a hierarchy is exported
- **THEN** the result is a flat interchange mesh that existing consumers accept unchanged

#### Scenario: Level generation is deterministic
- **WHEN** the same base mesh is subdivided to the same level twice, on any platform
- **THEN** the resulting positions and connectivity are identical

### Requirement: Detail is stored in a transported local frame
Detail SHALL be stored as coefficients in a local frame — tangent, bitangent and normal — derived from the subdivided parent surface, and SHALL NOT be stored only as a world-space offset.

A world-space offset is adequate for small changes and fails at the case the feature exists for: when the parent surface rotates or bends, a stored world vector no longer points along the surface it belongs to, and detail shears away from the form that carried it.

The frame SHALL be TRANSPORTED rather than rebuilt from whichever neighbour is encountered first: a UV tangent where a valid parametrization exists, a deterministic geometric tangent otherwise, rotated by the shortest arc when the parent normal moves, with sign consistency enforced against the previous frame. An unstable frame rotates detail, and the artefact appears in a render rather than in a numeric test.

A vertex's frame SHALL NOT depend on which patches are resident at its level.
The frame is built by rotating the parent frame's tangent onto the child's own
level normal, and at a region boundary that normal SHALL be summed over the
vertex's COMPLETE incident face set — the faces the level stores and the faces a
uniformly refined hierarchy would have stored there — in the same raw Newell
weighting as every other face of the level. The DISPLAY normal is the same sum
over the evaluated surface and SHALL take the same complete set, on the full
evaluation and on the partial one a stamp takes.

Without that, a one-sided sum puts a boundary vertex's frame up to 0.44 of a
unit normal away from the dense hierarchy's, and because a level's position is
its subdivided parent plus the frame applied to the detail, the same authored
coefficient reconstructs to a different world offset — so a guarantee that a
regional level holds a uniform hierarchy's numbers would hold only while the
detail at a boundary vertex is zero. With the complete set it holds for any
detail: the two hierarchies differ only by the order in which the same faces are
summed, which is float rounding and not a different answer.

Detail SHALL be authoritative in single precision. It SHALL NOT be quantized in this change: high-frequency detail is where a visible artefact appears first, and any compression waits on a measured error bound.

#### Scenario: A wrinkle survives a bend
- **WHEN** detail is sculpted at a fine level and the parent surface is then bent at a coarse one
- **THEN** the detail remains attached to the surface, in the same local orientation, rather than shearing away from it

#### Scenario: A small deformation does not flip the frame
- **WHEN** a parent surface is deformed slightly and the frames are rebuilt
- **THEN** no frame reverses sign, and the reconstructed detail does not rotate

#### Scenario: A region boundary reconstructs densely whatever its detail
- **WHEN** a regionally refined hierarchy and a uniformly refined hierarchy over the same cage are evaluated with no detail authored at the boundary
- **THEN** every vertex the two share holds the same position, bit for bit
- **WHEN** the same non-zero detail coefficients are then written into every vertex the regional level stores, boundary vertices included, and into the same vertices of the uniform hierarchy
- **THEN** every shared vertex reconstructs to the same position, the same frame and the same display normal, to within the rounding of summing the same faces in a different order

#### Scenario: A stroke below the boundary re-derives the boundary frames
- **WHEN** a detail coefficient is written at the level below a refined region, one vertex at a time, after the hierarchy has been evaluated
- **THEN** every vertex the refined level stores holds the position and display normal a full re-evaluation of the same hierarchy gives it, and the one the uniform hierarchy gives it

### Requirement: The sculpt level is independent of the display level
The library SHALL allow the level a brush writes to and the level a host displays to be set independently.

Editing a lower level while displaying a higher one SHALL reconstruct the higher levels through the stored relationship, preserving each level's persistent detail.

Sculpting at a level SHALL write only that level's persistent data. Higher levels' detail SHALL NOT be rewritten merely because their reconstructed world positions moved.

#### Scenario: A coarse edit is seen at the fine level
- **WHEN** the sculpt level is coarse, the display level is fine, and a broad edit is made
- **THEN** the displayed surface shows the broad change and retains the fine detail

#### Scenario: Returning to a fine level loses nothing
- **WHEN** detail is sculpted at a fine level, a coarse level is edited, and the fine level is made active again
- **THEN** the fine level's detail coefficients are unchanged

### Requirement: Propagation is local
A change at a level SHALL propagate to the descendants of the vertices it touched and SHALL NOT reconstruct whole levels.

Normals SHALL be recomputed for the changed region at the active level and for the propagated regions above it, never for a whole level.

Per-level runtime state — adjacency and the spatial index — SHALL be built lazily for the levels in use, keyed on the level's revision, and SHALL be droppable without touching authoritative detail.

#### Scenario: A dab does not rebuild a level
- **WHEN** a small dab is made at a coarse level of a deep hierarchy
- **THEN** the vertices reconstructed at each higher level are those descending from the touched vertices, and the cost is measured against a cold full reconstruction rather than asserted

#### Scenario: Dropping a cache costs nothing authoritative
- **WHEN** the runtime caches of inactive levels are released and then rebuilt
- **THEN** the reconstructed surface is identical and the detail checksum is unchanged

### Requirement: Adding a level reports its cost before paying it
Adding a level SHALL estimate the memory it will require — positions, detail, normals, indices, and any spatial index it would build — and SHALL refuse with a typed budget error rather than allocating part of it.

Adding and removing a level SHALL be build-then-publish: a failure or a cancellation SHALL leave the surface exactly as it was.

Subdivision multiplies faces, so it is the peak allocation rather than the steady state that fails on a memory-constrained device, and a host needs the number before it commits.

#### Scenario: An over-budget level is refused, not half-built
- **WHEN** a level whose predicted cost exceeds the budget is requested
- **THEN** the call fails with a typed error and the surface is unchanged

#### Scenario: A cancelled level leaves nothing behind
- **WHEN** adding a level is cancelled part way
- **THEN** the surface is byte-identical to before the call

### Requirement: A hierarchy carrying detail refuses arbitrary topology change
Once a hierarchy contains sculpted detail, an arbitrary change to its base connectivity SHALL be refused rather than applied.

The relationship between a level and its parent is defined by the subdivision stencils; changing the base connectivity makes every stencil above it meaningless, and detail stored against them is no longer interpretable.

The supported route SHALL be an explicit conversion: a new base, subdivided, with the previous surface's detail projected onto it. That conversion SHALL be a named operation with a stated cost, not an implicit consequence of an edit.

Geometry and detail projection SHALL be a separate operation from attribute transfer. Attribute transfer promises it moves no position, and that promise SHALL NOT be weakened to accommodate reprojection.

#### Scenario: A base edit is refused
- **WHEN** an arbitrary topology change to the base is attempted on a hierarchy that carries detail
- **THEN** it is refused with a typed error naming the conversion that is supported

#### Scenario: Detail survives an explicit reprojection
- **WHEN** a sculpted surface is projected onto a new base and a hierarchy is rebuilt from it
- **THEN** the reconstructed fine level matches the source within the stated tolerance

### Requirement: Multires undo records what was edited
The library SHALL record a multires gesture as the persistent data it changed — the level, the entries, and their values before and after — coalesced over the gesture.

Reconstructed positions at higher levels are derived state and SHALL NOT be recorded. Recording them would multiply an undo step by the number of levels and carry no information the relationship does not already supply.

Reverting SHALL restore the surface exactly, and the history SHALL reach the surface through a resolver supplied by the owner above it, as the existing kinds do.

#### Scenario: Undo size follows the edit, not the hierarchy
- **WHEN** a gesture at a coarse level of a deep hierarchy is recorded
- **THEN** the recorded bytes follow the vertices edited at that level and do not scale with the number of levels above it

#### Scenario: A multires stroke reverts exactly
- **WHEN** a stroke at any level is reverted
- **THEN** the surface reconstructs bit-identically to before the stroke

### Requirement: A cross-level neighbourhood is re-derived only when the level below has moved

The outside positions of a level's cross-level neighbourhood belong to the level
BELOW: they are the pure subdivision of that level's positions, so a stroke down
there moves them without anything the level above stores going stale. They SHALL
be re-derived whenever the level below's positions have changed since the last
time they were read, and SHALL NOT be re-derived when they have not.

Handing back an answer read before a change is forbidden: a reader completes its
boundary normals and frames from those positions and cannot distinguish a stale
answer from a current one.

The signal SHALL be the queue the level above already depends on. A level's
positions and the vertices it owes the level above are one fact, not two —
the neighbourhood's outside positions and the level's own subdivided positions
come from the same call on the same input — so a level's changed-vertex queue and
its positions revision SHALL move together through one door, and a writer SHALL
NOT be able to move one without the other. A revision maintained only where the
positions are ASSIGNED is insufficient and SHALL NOT be used: a stroke at level 0
is written into the cache's mesh by the brush and read out into the cage by the
hierarchy, so no assignment happens at that level and the level above would be
served a rim from before the stroke.

A write that RESTORES a level's positions to what its stored coefficients
reconstruct to SHALL NOT count as a change, because that is the value the current
revision already names.

The hierarchy SHALL report how many times a cross-level neighbourhood was asked
for and how many of those asks re-derived it, so the caching is observable as a
mechanism rather than inferred from values. A cache that has silently stopped
caching, and a cache that has silently stopped refreshing, SHALL both be
distinguishable from a correct one without reading a clock.

A level with no depth boundary SHALL be reported as no cross-level work at all:
every level of a uniform hierarchy is self-contained, holds no neighbourhood and
walks no rim, so both counts SHALL stay at zero there however the hierarchy is
sculpted. Otherwise "not zero" could not be read as "the region rim was asked
for", which is the whole use of the pair.

Releasing a neighbourhood, releasing a level's cache and rebuilding either SHALL
remain correct and SHALL err toward re-deriving: a level rebuilt from cold
produces the same positions, and re-reading them costs a walk rather than a wrong
answer.

#### Scenario: An interior dab does not walk the region rim
- **WHEN** a stroke is taken entirely inside a refined region, so no dab writes any level but the bound one
- **THEN** the bound level's cross-level neighbourhood is asked for on every dab and re-derived on none of them
- **AND** the outside positions it reports are the ones a hierarchy carrying the same detail and nothing cached would build

#### Scenario: A stroke on the level below re-derives the rim
- **WHEN** a stroke is taken at the level below a refined region, reaching vertices its outside positions are subdivided from
- **THEN** the neighbourhood is re-derived, its topology is unchanged, and its outside positions are the ones a hierarchy carrying the same detail and nothing cached would build

#### Scenario: A uniform hierarchy reports no cross-level work
- **WHEN** a stroke is taken on a hierarchy whose levels all store every patch, at the bound level and at the level below it
- **THEN** both counts stay at zero, while the same stroke on a hierarchy with a depth boundary counts asks

#### Scenario: A stroke on the cage re-derives the rim above it
- **WHEN** a stroke is taken at level 0, where the brush writes the level's mesh directly and the hierarchy reads those positions into the cage rather than writing them back
- **THEN** level 1's cross-level neighbourhood is re-derived, and its outside positions are the ones a hierarchy carrying the same detail and nothing cached would build

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
- **THEN** it too is summed over the complete incident face set, in the unweighted face sum that evaluator uses rather than the brush's angle weighting

#### Scenario: A smoothing verb is not dragged inward at a seam
- **WHEN** a smoothing stroke is applied across the boundary of a refined region, and the same stroke is applied to a uniformly refined hierarchy
- **THEN** the vertices the two hierarchies share finish in the same place, rather than the boundary being pulled into the refined region by a one-sided average
- **WHEN** the stroke is a detail-only smooth, which averages the stored coefficients, or a preserve-detail smooth, which averages the pure subdivision, over the same region of both hierarchies carrying the same coefficients on the vertices both store
- **THEN** every vertex the two share holds the same coefficients and the same position afterwards, the rim's neighbours on the coarser level counting as zero detail and as the pure subdivision respectively

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
