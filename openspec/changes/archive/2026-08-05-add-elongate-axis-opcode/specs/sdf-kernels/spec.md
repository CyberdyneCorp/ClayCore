# sdf-kernels — per-axis elongation reaches the tape

Delta for `add-elongate-axis-opcode`.

## ADDED Requirements

### Requirement: Per-axis elongation is a tape deformer
The tape SHALL carry per-axis elongation as a deformer opcode, translating the point outside the half-extents `h` toward the middle and leaving a flat plateau inside. It SHALL work for any primitive, symmetric or not, and SHALL compose in a deformer chain in authoring order.

It SHALL contribute no distance correction, SHALL expand the item's local bound by `h` per axis, and SHALL always downgrade the tracked field to a bound — the interior plateau is not a distance. Because the map is non-expansive it SHALL NOT reduce the safe step scale.

#### Scenario: An asymmetric primitive stretches
- **WHEN** a primitive that is not origin-symmetric is elongated per axis
- **THEN** the field matches the kernel's own map applied by hand, and the shape is stretched along the chosen axes

#### Scenario: Per-axis elongation is always a bound
- **WHEN** any primitive is elongated per axis
- **THEN** the tape reports a non-exact field, even for an origin-symmetric primitive, and the safe step scale is unchanged

#### Scenario: The bound contains the stretched geometry
- **WHEN** the deformed bound is computed
- **THEN** every point of the stretched surface lies inside it

#### Scenario: Device agreement
- **WHEN** the item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance
