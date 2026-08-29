# meshing — shared kernels and local cost

Delta for `add-shared-brush-kernels`.

## ADDED Requirements

### Requirement: Brush deformation math is representation-neutral
The deformation math behind the mesh verbs SHALL live behind an interface that names no mesh, no adjacency structure and no vertex numbering — a span of positions, normals and weights, a neighbourhood view, the stamp's frame, and the plane a flatten-family verb was given or computed.

Every sculptor the library gains SHALL call those kernels rather than reimplement them, so that a verb means the same thing on every representation that offers it. Where a representation cannot offer a verb, it SHALL omit the verb rather than approximate it.

Extracting the kernels SHALL NOT change what the fixed-topology sculptor produces. The results SHALL be compared BIT FOR BIT against the pre-extraction implementation rather than within a tolerance, because a tolerance would admit a reordered accumulation and that is the mistake this refactor is most likely to make.

#### Scenario: The fixed path is unchanged by the extraction
- **WHEN** every verb is applied to the golden fixtures before and after the kernels are extracted
- **THEN** the resulting positions, normals and colours are byte-identical

#### Scenario: A kernel names no representation
- **WHEN** the kernel interface is compiled against a translation unit that includes no mesh, adjacency or BVH header
- **THEN** it compiles

### Requirement: A stamp allocates and scans for what it touches
A brush stamp SHALL gather a WORKSET whose capacity tracks the largest recent brush footprint and SHALL NOT allocate, clear or scan storage proportional to the total surface.

The workset SHALL distinguish the READ HALO from the WRITE REGION. Verbs that average over a one-ring read vertices they do not move, and a dirty report SHALL name the write region only, so a host does not upload geometry that did not change.

After warm-up, an ordinary local stamp on a mesh whose topology is stable SHALL perform no heap allocation. Growth on first encountering a larger footprint is permitted; steady repeated local sculpting is not.

A multi-pass verb SHALL query the spatial index once per stamp and iterate over local buffers, rather than re-querying per pass.

#### Scenario: Warm stamps do not allocate
- **WHEN** a stroke of many stamps of similar footprint runs after the first stamp has warmed the scratch
- **THEN** the instrumented allocation count for the subsequent stamps is zero

#### Scenario: The dirty report excludes the read halo
- **WHEN** a smoothing verb runs and reports the vertices it changed
- **THEN** the report names the vertices it moved and not the ring it only read
