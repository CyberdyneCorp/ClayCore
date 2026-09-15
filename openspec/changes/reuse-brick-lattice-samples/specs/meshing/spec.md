## ADDED Requirements

### Requirement: Exact reuse of ordinary brick lattice samples
The mesher SHALL preserve every sampled float bit and the complete mesh output when reusing shared lattice points within an ordinary brick.

#### Scenario: Shared cell corners
- GIVEN an ordinary brick of dimension 1 through 16
- WHEN its cells are recorded with sample reuse
- THEN each point of its closed lattice SHALL be read exactly once from the original sampler
- AND repeated corner references SHALL receive the same stored bits

#### Scenario: Boundary and subset output
- GIVEN a full or subset brick request with neighboring, missing or uniform bricks
- WHEN sample reuse is enabled
- THEN positions, indices, normals, colors and brick ranges SHALL match the independent reference recorder exactly
- AND boundary attribution and LOD behavior SHALL remain unchanged

#### Scenario: Bounded independent storage
- GIVEN concurrent brick recording
- WHEN sample blocks are allocated
- THEN each worker SHALL use at most 19,652 bytes of sample payload per active block
- AND blocks SHALL share no mutable state or retain data across mesh calls
