# sdf-kernels — wrap_around reaches the tape

Delta for `add-wrap-around-opcode`.

## ADDED Requirements

### Requirement: wrap_around is a tape deformer
The tape SHALL carry `wrap_around` as a deformer opcode, bending the local X interval `[x0, x1]` around a cylinder about the Z axis so that a flat item becomes a wrapped one. It SHALL compose with the other deformers in authoring order like any chain member.

Its influence SHALL be bounded by the disc the wrap sweeps: with `r = (x1 - x0) / 2pi`, the deformed local bound is `|x|, |y| <= max(|r + ymin|, |r + ymax|)` over the content's radial extent, with `z` unchanged. Because the deformer is a metric breaker, it SHALL downgrade the node's tracked field info and contribute a Lipschitz factor derived from the content's radial extent, so sphere tracing slows rather than tunnelling.

#### Scenario: A wrapped item bends around the cylinder
- **WHEN** an item spanning the wrap interval is evaluated after the deformer
- **THEN** its surface lies about the cylinder of radius `r`, and the tape agrees with the kernel's own `cwrap_around_point` composed with the primitive

#### Scenario: The bound contains the wrapped geometry
- **WHEN** the deformed bound is computed for a wrapped item
- **THEN** every point of the wrapped surface lies inside it

#### Scenario: Wrapping is not exact
- **WHEN** a wrapped item is compiled
- **THEN** the tape reports a non-exact field and a safe step scale below 1

#### Scenario: Device agreement
- **WHEN** a wrapped item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance
