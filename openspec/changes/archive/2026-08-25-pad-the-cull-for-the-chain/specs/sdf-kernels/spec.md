# sdf-kernels — pad a cull for the chain, not just for one blend

Delta for `pad-the-cull-for-the-chain`.

## ADDED Requirements

### Requirement: A culled tape agrees with the full tape inside the band
Compiling against a culling region SHALL produce a tape whose band-clamped values are identical to the full tape's at every point inside that region. A consumer that culls does so to go faster, not to get a different field, and the surface lies inside the band.

Culling SHALL account for every way an item can reach a value beyond its own influence bound. Two do. A feathered replace crossfades, and steers a value from up to its volume's band away. A SMOOTH-UNION CHAIN is the subtler one: an item's bound is dilated by what a single blend can move, but the accumulated value part way down a chain sits well above where that chain ends up, so an item whose final contribution is nothing can still lie within the blend radius of the RUNNING value and change it. The reach therefore grows with the length of the chain rather than being a property of the item.

Accounting for this SHALL be the COMPILER's responsibility rather than the caller's, so that a consumer dilating its region by the band alone — which is what the region is documented to be — gets the guarantee without knowing why. A hard union has no such term: the minimum is exact and associative, and dropping an item that does not win it changes nothing.

Where no fixed dilation can bound the reach, the compiler SHALL apply the largest reach any single item in the layer declares, and the limits of that SHALL be stated where the contract is stated rather than left as an implied proof.

#### Scenario: A long smooth-union chain culls without moving the field
- **GIVEN** a document whose surface is built from a long chain of smoothly-unioned items
- **WHEN** a region of it is compiled against a culling region dilated by the band alone, and evaluated inside that region
- **THEN** every value within the band equals the full tape's exactly

#### Scenario: A short chain and a hard union agree too
- **WHEN** the same comparison is made on a handful of blended items, and on the same shapes unioned hard
- **THEN** both agree exactly, as they did before

#### Scenario: The caller does not compensate
- **WHEN** a consumer builds its culling region by dilating a brick by its band and nothing else
- **THEN** the guarantee holds, because the compiler adds what the chain needs
