# examples — the bridge is measured, not asserted

Delta for `rasterize-mesh`.

## ADDED Requirements

### Requirement: A gallery entry for the mesh-to-voxel bridge
The gallery SHALL gain an entry rasterizing an imported model straight to cells, with committed renders, run by `examples/run_all.py` like every other entry.

It SHALL measure the direct path AGAINST the four-step detour it replaces rather than describing the difference: on a thick model, where the two agree; on a feature thinner than two cells, where they do not; and on colour, which the detour cannot carry.

It SHALL rasterize a model with a hole in it on purpose, and report that the solid survives, because the sign choice is the reason the entry point looks the way it does.

It SHALL end by applying voxel sculpting verbs to the imported model, since reaching them without a document is the point of the trip. The edit SHALL be firm enough to see in the render — an edit that moves thirty cells out of forty thousand is real and invisible, and a render nobody can read is not evidence.

#### Scenario: The gallery runs
- **WHEN** `python examples/run_all.py` runs
- **THEN** the entry runs to completion, writes its renders, and its self-checks pass

#### Scenario: A reader can see what one sampling bought
- **WHEN** a reader opens the side-by-side render
- **THEN** the direct path carries the model's colour and the detour does not
