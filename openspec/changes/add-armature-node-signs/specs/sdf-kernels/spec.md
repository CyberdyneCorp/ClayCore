# sdf-kernels — a sign per armature node

Delta for `add-armature-node-signs` (#99).

## MODIFIED Requirements

### Requirement: An armature is a tree of spheres
The tape SHALL provide an opcode carrying a set of nodes, each a position and a radius, a parent index per node, and a sign per node, +1 or -1, and SHALL evaluate the field as the armature of the positive nodes MINUS the armature of the negative nodes, each half built exactly as the unsigned armature is.

A link SHALL exist only between two nodes of the same sign — skin between builders, carve between carvers — and a link whose ends disagree SHALL belong to neither half: a node whose parent carries the other sign reads as a root of its own half. This is the membrane cut stated structurally — skin along a negative node's links is never drawn, rather than drawn and patched — and it is what keeps a carve from sweeping a positive parent's radius, which would swallow the head an eye-socket child is cut into.

A segment SHALL use the same construction the stroke opcode already uses: a round cone between two radii, a capsule where the radii agree, and a sphere where the endpoints coincide. Positive segments SHALL be combined with the same smooth union, whose radius is a parameter of the armature; negative segments SHALL be subtracted with the matching smooth subtraction at the same radius, hard where the radius is zero, and the whole carve SHALL come after the whole positive fold, so a sleeve from any other branch that runs through a hollow is cut and no union can re-fill it.

A root — a node whose parent is itself, or whose parent carries the other sign — SHALL contribute its own sphere ONLY when no other node of its half names it as a parent, and the rule SHALL be applied per half: a root with same-sign children is already contained in every link that names it, and contributing it twice is harmless under a hard fold but wrong under a soft one — the smooth union of two overlapping terms pulls the surface outward, and the smooth subtraction of two overlapping terms over-carves the same way.

This is the chain opcode generalised from consecutive pairs to parent pairs. An armature whose parents form a line and whose signs are all positive SHALL therefore evaluate identically to the stroke with the same points, which is what keeps the two from drifting; an all-positive armature SHALL evaluate identically to one that predates signs.

#### Scenario: A chain armature is a stroke
- **WHEN** an armature whose every node's parent is the node before it, all signs positive, is evaluated, and a stroke is built from the same points and blend
- **THEN** the two fields agree everywhere, within the parity tolerance

#### Scenario: A branch is a union, not a chain
- **WHEN** two children share one parent and a point is sampled near each child
- **THEN** both children contribute material, and neither is joined to the other except through the parent

#### Scenario: A single node is a sphere
- **WHEN** an armature has one node, whose parent is itself
- **THEN** the field is that sphere, and nothing is degenerate

#### Scenario: A root that has children contributes no extra sphere
- **WHEN** a chain armature is evaluated with a non-zero blend
- **THEN** it still agrees with the stroke, because the root's sphere already lies inside the link that names it and adding it again would pull the smooth union outward

#### Scenario: A negative node's links carry no skin
- **WHEN** a chain runs A, B, C with B negative, and points are sampled between A and B and between B and C
- **THEN** no sleeve is drawn through the hollow in either direction: A and C keep their own material, and B's ball carves whatever else overlaps it

#### Scenario: A negative child does not swallow its positive parent
- **WHEN** a small negative child is placed inside a large positive parent — an eye socket in a head
- **THEN** the carve is the child's own sphere, the parent's skin survives everywhere the carve does not reach, and no internal void is cut beyond it

#### Scenario: A negative subtree carves as one rig
- **WHEN** two negative nodes in a parent-child pair sit inside positive material
- **THEN** their link is carved as the same sphere-swept segment the positive half skins with, so the hollow is one swept scoop rather than two disjoint balls

#### Scenario: An all-negative armature is empty
- **WHEN** every node of an armature is negative
- **THEN** the field is empty — there is nothing to carve from — and nothing is degenerate

### Requirement: A branch folds in a stated order
The smooth union is not associative, and neither is the subtraction that follows it, so the order SHALL be deterministic and stated: positive segments fold in ascending node index, then negative segments subtract in ascending node index, so that the same armature evaluates identically on every backend and in every process.

#### Scenario: The same armature gives the same field everywhere
- **WHEN** an armature with a branching node is evaluated on every registered backend
- **THEN** the results agree within the parity tolerance

#### Scenario: The same signed armature gives the same field everywhere
- **WHEN** an armature holding negative nodes — one a root of its half, one a negative parent-child link — is evaluated on every registered backend
- **THEN** the results agree within the parity tolerance
