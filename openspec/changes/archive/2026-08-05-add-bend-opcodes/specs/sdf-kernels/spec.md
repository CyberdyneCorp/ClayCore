# sdf-kernels — the ramped deformers reach the tape

Delta for `add-bend-opcodes`.

## ADDED Requirements

### Requirement: Ramped bends are tape deformers
The tape SHALL carry `bend_linear` and `bend_radial` as deformer opcodes. `bend_linear` SHALL displace the point by a vector ramped along the segment between two points; `bend_radial` SHALL displace along Y by an amount ramped across a radial band. Both SHALL honour an easing curve, and both SHALL compose in a deformer chain in authoring order.

Both SHALL dilate the item's local bound by the displacement they can apply, and SHALL downgrade the tracked field info with a Lipschitz factor equal to the ramp's slope — the displacement over the span it ramps across.

#### Scenario: A linear ramp displaces only across its span
- **WHEN** a point before the segment start, one after its end, and one midway are evaluated
- **THEN** the first is undisplaced, the last is displaced by the full vector, and the middle by the eased fraction — matching the kernel applied by hand

#### Scenario: A radial ramp displaces only across its band
- **WHEN** points inside `r0`, beyond `r1`, and between them are evaluated
- **THEN** the displacement is zero, full, and the eased fraction respectively

#### Scenario: The easing curve reaches the field
- **WHEN** the same bend is built with two different easing curves
- **THEN** the fields differ within the ramp span

#### Scenario: The bound contains the displaced geometry
- **WHEN** the deformed bound is computed for either bend
- **THEN** every point of the displaced surface lies inside it

#### Scenario: Existing documents still load
- **WHEN** a document saved before these opcodes existed is read
- **THEN** its deformers decode exactly as before, because the reader takes its parameter count from the deformer type

#### Scenario: Device agreement
- **WHEN** an item using either bend is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance
