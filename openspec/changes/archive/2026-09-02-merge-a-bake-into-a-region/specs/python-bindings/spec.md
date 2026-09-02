## ADDED Requirements

### Requirement: A region of a layer can be merged from pyclay
`Layer.consolidate_region` SHALL bake the influence closure of a region and install it in the absorbed items' place, reporting the bake's cost alongside how many roots it absorbed and whether the closure took the whole layer. `Layer.plan_region_merge` SHALL report the same without baking.

#### Scenario: The rest stays parametric
- **WHEN** a region over one of four separated items is merged
- **THEN** one is absorbed, the item count is unchanged, and the surface outside the closure has not moved

#### Scenario: Repeated merges do not stack
- **WHEN** the same patch is merged six times
- **THEN** the item count is what it was after the first
