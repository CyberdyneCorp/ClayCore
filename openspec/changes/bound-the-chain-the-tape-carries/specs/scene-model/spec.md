## ADDED Requirements

### Requirement: A tape is bounded by the chain it carries

A compiled tape's declared Lipschitz bound SHALL be derived from the deformers
THAT TAPE EMITS, and SHALL NOT include a deformer the compile dropped.

A culled compile drops every finite-support warp whose support cannot reach the
region, on the ground that over that region it is the identity. An identity
contributes a factor of one. Charging for it declares a bound for a chain that
is not in the tape being bounded — a different, longer chain — and the number
grows with the length of that other chain rather than with anything the tape
does.

The bound SHALL remain an upper bound for the tape's field WHEREVER that field
can be evaluated, not merely inside the region the compile was culled to. A
march may begin outside the region and cross it, and the culled tape's field
outside the region is its own field rather than the document's.

A whole-document compile SHALL be unaffected: with no region to test against
nothing is dropped, so the emitted chain and the item's chain are the same
chain.

This SHALL hold as a property of the emitted tape rather than of the order in
which the compiler visits an item. A bound taken from the wrong item's chain
could be SHORTER than the truth, and a bound below the truth does not cost
frames — it steps the marcher through the surface.

#### Scenario: A region no warp reaches declares no warp's bound
- **WHEN** a tape is compiled to a region that no finite-support warp on the item can reach
- **THEN** it carries no warps and declares a bound of exactly one, with a full safe step scale

#### Scenario: Unreachable warps do not raise the bound
- **GIVEN** a region no warp on the item can reach
- **WHEN** further warps are added elsewhere on the item
- **THEN** the bound declared for that region does not move

#### Scenario: A whole-document compile still compounds
- **WHEN** warps are added to an item and the document is compiled with no cull region
- **THEN** the declared bound grows, because every warp is emitted

#### Scenario: The tightened bound is still a bound
- **WHEN** a culled tape is marched by its own declared step
- **THEN** the hits agree with the surface crossings found by walking the ray in fixed increments, neither missing one nor reporting one that is not there
