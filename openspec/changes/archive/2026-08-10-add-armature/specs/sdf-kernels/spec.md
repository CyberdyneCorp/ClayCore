# sdf-kernels — armatures

Delta for `add-armature`.

## ADDED Requirements

### Requirement: An armature is a tree of spheres
The tape SHALL provide an opcode carrying a set of nodes, each a position and a radius, and a parent index per node, and SHALL evaluate the field as the union of one sphere-swept segment per node-parent pair.

A segment SHALL use the same construction the stroke opcode already uses: a round cone between two radii, a capsule where the radii agree, and a sphere where the endpoints coincide. The segments SHALL be combined with the same smooth union, whose radius is a parameter of the armature.

A root — a node whose parent is itself — SHALL contribute its own sphere ONLY when no other node names it as a parent. A root with children is already contained in every link that names it, and contributing it twice is harmless under a hard union but wrong under a soft one: the smooth union of two overlapping terms pulls the surface outward, so a chain armature would stop matching the stroke it must equal.

This is the chain opcode generalised from consecutive pairs to parent pairs. An armature whose parents form a line SHALL therefore evaluate identically to the stroke with the same points, which is what keeps the two from drifting.

#### Scenario: A chain armature is a stroke
- **WHEN** an armature whose every node's parent is the node before it is evaluated, and a stroke is built from the same points and blend
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

### Requirement: A branch folds in a stated order
The smooth union is not associative, so three or more links meeting at one node give a field that depends on the order they are combined. The order SHALL be deterministic and stated, so that the same armature evaluates identically on every backend and in every process.

#### Scenario: The same armature gives the same field everywhere
- **WHEN** an armature with a branching node is evaluated on every registered backend
- **THEN** the results agree within the parity tolerance

### Requirement: An armature declares its exactness
A smooth union of round cones is a bound rather than an exact distance, and an armature SHALL declare that and contribute the Lipschitz factor the smooth union implies, exactly as the chain opcode already does.

#### Scenario: The step scale reflects the blend
- **WHEN** an armature's blend radius is raised
- **THEN** the reported safe step scale reflects it, and sphere tracing does not overshoot the surface
