## ADDED Requirements

### Requirement: A magnify on the assembled surface is reachable from pyclay
pyclay SHALL expose `Layer.magnify_surface` and `Layer.magnify_surface_preview` alongside `Layer.move_surface`, returning the nodes that took a warp, making the whole gesture one undo step, and refusing a zero strength or a non-positive radius with a ValueError.

#### Scenario: A blended form swells as one surface
- **WHEN** Layer.magnify_surface is called over a form built from two blended items
- **THEN** both items are named in the result and the surface swells symmetrically about the centre

#### Scenario: The preview is pure
- **WHEN** Layer.magnify_surface_preview is called
- **THEN** it names the nodes the apply would touch and the field is bit-for-bit unchanged
