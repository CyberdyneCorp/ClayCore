## ADDED Requirements

### Requirement: A bake can be merged into a region of a layer
A host SHALL be able to bake a REGION of a layer into one volume and put it back where the items it absorbed were, leaving every item outside parametric. Collapsing the whole layer SHALL NOT be the only way to install a bake.

The region a merge absorbs and samples SHALL be the INFLUENCE CLOSURE of the caller's region: the region grown, to a fixed point, until every item whose influence bound meets it is wholly inside it. Absorbing merely the items that overlap the caller's region is NOT sufficient — an item straddling the edge remains, and material it had carved returns where the installed volume cannot remove it again.

At the closure, the field outside the box SHALL be unchanged, and inside the box the bake SHALL be the whole answer, because no remaining item contributes there.

An item whose influence is unbounded SHALL pull the closure out to the whole layer. Where the closure reaches every visible root the operation IS a whole-layer consolidation, and that SHALL be reported rather than hidden.

The merge SHALL be ONE undo step whose inverse restores the absorbed items with their ids, parameters, colours and deformers, SHALL leave hidden items alone, and SHALL be refused on a protected layer before anything is sampled.

A host SHALL be able to ask what a merge would absorb, and over what box, without baking anything.

#### Scenario: The closure takes what the region reaches and no more
- **WHEN** a region is planned over one of several well-separated items
- **THEN** it absorbs that item alone, and the sampled box does not reach the others

#### Scenario: The closure is a fixed point, not one pass
- **WHEN** an item earlier in the edit list is only reached after a later item widens the box
- **THEN** it is absorbed too

#### Scenario: The field outside the merged region does not move
- **WHEN** a region of a layer is merged
- **THEN** the surface outside the closure is where it was, and the merged region still has its own surface

#### Scenario: Working one patch repeatedly does not stack volumes
- **WHEN** the same patch is merged once per gesture over several gestures
- **THEN** the layer holds one baked item for that patch, not one per gesture

#### Scenario: A whole-layer closure says so
- **WHEN** the closure reaches every visible root
- **THEN** the merge reports that it consolidated the layer

#### Scenario: It undoes to the parametric form
- **WHEN** a region merge is undone
- **THEN** the absorbed items are back and the field is what it was
