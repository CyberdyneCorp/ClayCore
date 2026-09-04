## MODIFIED Requirements

### Requirement: An imported mesh's field is affordable to query

Distance and inside/outside queries against an imported mesh SHALL be answered
through a hierarchy that SUMMARIZES distant geometry rather than by consulting
every triangle, so that a mesh with ten times the triangles does not cost ten
times the work to query. Without it an ordinary import is unusable.

**This SHALL be gated on WORK DONE rather than on time taken.** The property is
that the walk stops at a distant node and answers for its whole subtree, which
is a count of nodes visited and triangles tested — a number identical on every
machine. Asserted as a ratio of wall clocks it becomes a claim about the
scheduler instead: both sides of such a comparison run in well under a
millisecond, one preemption inside the smaller side inflates the ratio, and the
gate has twice failed on one platform with the hierarchy unchanged and once on a
change that provably could not affect it.

The instrumentation SHALL cost nothing when nothing is measuring. A query path
that picking and meshing depend on SHALL NOT carry a permanent charge so that a
test can read a counter.

The gate SHALL be proven against its own regression: with summarizing disabled,
the same measurement SHALL rise to approximately the ratio of the triangle
counts, so that a gate which had stopped testing anything is visible as a gate
that no longer fails when it should.

#### Scenario: A denser mesh does not cost proportionally more
- **WHEN** the same sweep of queries runs against a mesh and against one with an order of magnitude more triangles
- **THEN** the work the walk performs grows by far less than the triangle count does

#### Scenario: The gate fails when summarizing is off
- **WHEN** the same comparison is made with summarizing disabled
- **THEN** the work ratio rises to approximately the triangle ratio, and the gate's bound is exceeded

#### Scenario: Measuring is free when nobody measures
- **WHEN** queries run with no counter attached
- **THEN** they cost what they cost without the instrumentation present
