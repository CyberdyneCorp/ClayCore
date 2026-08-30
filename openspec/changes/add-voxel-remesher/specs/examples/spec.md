# examples — voxel remesh

Delta for `add-voxel-remesher`.

## ADDED Requirements

### Requirement: A gallery entry for voxel remeshing
The gallery SHALL carry a numbered example for the global voxel remesh, with committed renders, run by `examples/run_all.py` like every other entry.

It SHALL show the operation doing the thing it exists for rather than only that it runs: a source whose topology is stretched past usefulness, and a source of two intersecting shells that the remesh fuses into one body. The triangle counts and the component counts before and after SHALL be printed, because "the topology is replaced" and "the shells fused" are the claims and a number is how a reader checks them.

It SHALL render the same model at two resolutions so that what a coarser voxel size costs is visible rather than described, and SHALL print the preflight estimate beside the resolution it belongs to, since the estimate is what a host would put in front of an artist.

It SHALL state in its output that details finer than the voxel size are lost and that UVs are dropped, because those are the two properties a user is most likely to discover the hard way.

#### Scenario: The gallery runs
- **WHEN** `python examples/run_all.py` runs
- **THEN** the entry runs to completion, writes its renders under the gallery's output directory, and its self-checks pass

#### Scenario: A reader can see what fused
- **WHEN** a reader opens the intersecting-shells render and its printed output
- **THEN** the source's two shells and the result's single body are visible, and the printed component counts say two before and one after
