# python-bindings — loft

Delta for `add-loft-opcode`.

## ADDED Requirements

### Requirement: Lofts from Python
The module SHALL expose a loft taking two or more profiles and a half-depth, with an easing curve over the interpolation.

#### Scenario: Lofting a circle to a box
- **WHEN** a script lofts a circle to a box and evaluates the ends
- **THEN** each end matches its own profile
