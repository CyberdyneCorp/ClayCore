## ADDED Requirements

### Requirement: Regional volume maintenance does not expand its own workload

Regional consolidation SHALL retain unaffected samples when a compatible
additive volume's local edits can be replaced independently. The rebuilt
region SHALL include the requested region, the support of every removed
deformer, and the sampling transition halo. It SHALL NOT expand merely to
include the retained volume's conservative deformed extent.

#### Scenario: Repeated maintenance of a stationary patch
- **GIVEN** disjoint subtools and local Move edits on one previously baked subtool
- **WHEN** the same patch is consolidated repeatedly at the same resolution
- **THEN** field sampling and redistancing remain bounded by that patch and its local edit support
- **AND** unrelated parametric roots remain unchanged and scene roots do not accumulate

#### Scenario: A retained volume crosses the patch boundary
- **WHEN** a compatible volume is partially consolidated
- **THEN** untouched stored samples and colours are retained
- **AND** a transition outside the edited support joins the rebuilt field without introducing an artificial box surface
- **AND** the resulting sampled field declares a conservative slope bound

#### Scenario: Removing edits outside the last stroke
- **GIVEN** several finite-support deformers on a retained volume
- **WHEN** the caller requests only the last stroke's region
- **THEN** the bake includes every removed deformer’s support

#### Scenario: Partial replacement cannot preserve the fold
- **GIVEN** incompatible modifiers, sampling settings, or overlapping operands
- **WHEN** a regional consolidation is requested
- **THEN** the operation uses the conservative whole-root closure and reports the plan it executes

#### Scenario: A partial bake is reversible and cancellable
- **WHEN** a partial bake completes
- **THEN** one undo restores the original samples, deformers, and sharing, and redo restores the result
- **AND** saving and reloading preserves the resulting volume
- **WHEN** a bake is cancelled before publication
- **THEN** the document and its undo history remain unchanged

## MODIFIED Requirements

### Requirement: A bake can be merged into a region of a layer
A host SHALL be able to bake a REGION of a layer into one volume and put it back where the items it absorbed were, leaving every item outside parametric. Collapsing the whole layer SHALL NOT be the only way to install a bake.

Except for a compatible retained-volume patch as specified below, the region a merge absorbs and samples SHALL be the INFLUENCE CLOSURE of the caller's region: the region grown, to a fixed point, until every item whose influence bound meets it is wholly inside it. Absorbing merely the items that overlap the caller's region is NOT sufficient — an item straddling the edge remains, and material it had carved returns where the installed volume cannot remove it again.

At the closure, the field outside the box SHALL be unchanged, and inside the box the bake SHALL be the whole answer, because no remaining item contributes there.

An item whose influence is unbounded SHALL pull the closure out to the whole layer. Where a whole-root closure reaches every visible root the operation IS a whole-layer consolidation, and that SHALL be reported rather than hidden. A retained-volume patch SHALL report `whole_layer = false`, including on a single-root layer, because unaffected samples remain outside the bake.

The merge SHALL be ONE undo step whose inverse restores the absorbed items with their ids, parameters, colours and deformers, SHALL leave hidden items alone, and SHALL be refused on a protected layer before anything is sampled.

A host SHALL be able to ask what a merge would absorb, and over what box, without baking anything. The preview planner SHALL assume the existing volume spacing and band; if the actual request changes these settings, the returned execution report SHALL describe its conservative whole-root fallback. The cost report SHALL describe the installed volume, including retained storage; the merge box SHALL describe the sampled patch.

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
- **WHEN** a whole-root closure reaches every visible root
- **THEN** the merge reports that it consolidated the layer

#### Scenario: It undoes to the parametric form
- **WHEN** a region merge is undone
- **THEN** the absorbed items are back and the field is what it was
