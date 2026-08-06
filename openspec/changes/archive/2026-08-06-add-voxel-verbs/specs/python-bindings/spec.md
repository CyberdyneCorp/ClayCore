# python-bindings — the remaining voxel verbs

Delta for `add-voxel-verbs`.

## ADDED Requirements

### Requirement: The new verbs from Python
The module SHALL expose fill-cavities, scrape, smudge and carve-with-alpha, taking the alpha as an (H, W) array.

#### Scenario: Carving with an alpha array
- **WHEN** a script carves with an (H, W) alpha that is opaque on one half
- **THEN** material is removed under the opaque half only
