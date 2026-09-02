## MODIFIED Requirements

### Requirement: A mask gates an item's participation
An item SHALL be able to carry a mask that scales how much it contributes at each point. Where the mask is fully protective the accumulated field SHALL be exactly what it was before the item; where the mask is absent the result SHALL be exactly the unmasked result.

This SHALL apply to every combine operation, including booleans, not only to edits made through the stroke engine.

A gate SHALL be evaluated in WORLD space. The mask it is measured from is stored in world units on its own lattice, so the region it protects is where it was painted and SHALL NOT be moved, turned or scaled by the transform of the item it gates, nor by that item's layer. A gate placed by the gated item's own transform protects a region that moves with the very item it holds back, which is indistinguishable from a gate that does nothing.

#### Scenario: A fully protected region is untouched by a subtract
- **WHEN** a subtract item overlaps a region whose mask is fully protective
- **THEN** the field in that region is exactly what it was without the item

#### Scenario: An unprotected region is unaffected by the mask's presence
- **WHEN** an item is masked but the mask is zero over the evaluated region
- **THEN** the field is exactly the unmasked result

#### Scenario: The gated item is placed
- **WHEN** a gated item is moved, turned or scaled in a way that leaves what it carves unchanged
- **THEN** the protected region is the same one it was at identity

#### Scenario: The gated item's layer is placed
- **WHEN** the layer holding a gated item is turned onto its own footprint
- **THEN** the protected region is the same one it was at identity

### Requirement: A gated item declares its Lipschitz cost
Mixing two fields by a spatially varying weight is not a distance field. The tape SHALL charge the mix against the item's Lipschitz bound, derived from the mask's own gradient bound, so raymarching a gated document does not overshoot.

The falloff width SHALL be charged in world units, as given. It is already a world quantity and the gate is read in world space, so applying the layer's scale to it would declare a softness the kernel does not have — and because a wider gate costs LESS, doing so overstates the safe step scale rather than understating it.

#### Scenario: A uniform mask costs no step scale
- **WHEN** an item's mask is constant over its influence
- **THEN** the safe step scale is what the same item unmasked reports

#### Scenario: Raymarching a gated document does not overshoot
- **WHEN** a gated item's document is marched by its declared step scale
- **THEN** no step lands past the surface

#### Scenario: The same world split differently between layer and item scale
- **WHEN** two documents describe the same geometry and the same gate, one with a unit layer scale and one with the scale moved onto the layer and divided out of every item
- **THEN** both declare the same safe step scale, and marching either by it does not overshoot
