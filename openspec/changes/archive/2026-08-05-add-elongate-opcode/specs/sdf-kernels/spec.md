# sdf-kernels — elongate reaches the tape

Delta for `add-elongate-opcode`.

## ADDED Requirements

### Requirement: Elongation is a tape deformer
The tape SHALL carry elongation as a deformer opcode, inserting flat sections of half-extent `h` along each axis so a shape stretches without its ends distorting. It SHALL both warp the evaluation point and contribute the corresponding distance correction, and SHALL compose in a deformer chain in authoring order.

Elongation SHALL expand the item's local bound by `h` per axis. Because the map is non-expansive it SHALL NOT reduce the safe step scale; it SHALL preserve the tracked exactness when the elongated primitive is origin-symmetric, and downgrade to a bound otherwise.

#### Scenario: A stretched shape keeps its ends
- **WHEN** a sphere is elongated along one axis
- **THEN** the result is a capsule: the field matches the kernel's own elongation applied by hand, and the caps are undistorted

#### Scenario: Elongation of a symmetric primitive stays exact
- **WHEN** an origin-symmetric primitive is elongated
- **THEN** the tape reports an exact field and a safe step scale of 1

#### Scenario: Elongation of an asymmetric primitive is a bound
- **WHEN** a primitive that is not origin-symmetric is elongated
- **THEN** the tape reports a non-exact field, because the correction is only valid about the origin

#### Scenario: The bound contains the stretched geometry
- **WHEN** the deformed bound is computed for an elongated item
- **THEN** every point of the stretched surface lies inside it

#### Scenario: Device agreement
- **WHEN** an elongated item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance
