# examples Specification

## Purpose
The gallery, which is how a capability proves it works to a human rather than to
a test runner.

Every feature area has a numbered example that RUNS in CI, asserts what it
claims, and commits its output — so a change that breaks a picture is caught
with everything else rather than discovered when someone next looks. The
constraint that they depend on nothing beyond the wheel and numpy is what keeps
them runnable by a reader who has just installed it.

## Requirements

### Requirement: Every feature area has a runnable example
The repository SHALL ship an `examples/` directory whose scripts collectively exercise the documented SDF vocabulary — primitives, blend kinds, extended combine modes, deformers, repetition, profile lifts, transitions and strokes — and the voxel sculpting surface — brush and box and line edits, mirroring, flood select, palettes, greedy meshing and SDF rasterization. Each script SHALL be executable standalone and SHALL write its outputs to `examples/output/`.

#### Scenario: Running an example produces artifacts
- **WHEN** a user runs any script in `examples/` from a checkout with `pyclay` installed
- **THEN** it exits zero and writes at least one PNG or model file under `examples/output/`

#### Scenario: A feature with no example is caught
- **WHEN** the gallery's coverage check runs
- **THEN** it reports any primitive class exposed by the module that no example instantiates

### Requirement: No dependencies beyond the wheel and numpy
Examples SHALL import only `pyclay`, `numpy`, and the Python standard library. Image output SHALL be produced by a bundled PNG writer rather than an imaging dependency.

#### Scenario: Bare environment
- **WHEN** the examples run in an environment with only `pyclay` and `numpy` installed
- **THEN** every script completes without an import error

### Requirement: Outputs are committed and reproducible
Rendered PNGs and exported models SHALL be committed under `examples/output/` so the gallery is viewable without running anything, and SHALL be regenerable by a single runner script.

#### Scenario: Regenerating the gallery
- **WHEN** a contributor runs the examples runner after changing a kernel
- **THEN** the committed outputs are overwritten in place, and the diff shows which features changed appearance

### Requirement: Examples run in CI
CI SHALL execute every example script and fail the build on a non-zero exit, so an API or behaviour change that breaks an example is caught in the same way a broken test is.

#### Scenario: A renamed binding breaks the gallery
- **WHEN** a Python binding is renamed without updating the examples
- **THEN** the examples CI job fails naming the script that raised

### Requirement: The mesh brush gallery
The examples gallery SHALL gain entries covering the fixed-topology mesh brushes, with committed renders, run by `examples/run_all.py` like every other entry.

The M1 verbs SHALL be shown on an imported model AND on a quad-exported re-import, so the claim that a quad mesh survives sculpting is visible rather than asserted.

Each M2 verb SHALL be shown doing the thing its name promises: `clay`'s flat-topped strips against `draw`'s swell, `crease`'s closed fold, `polish` keeping a hard edge while a plain `smooth` at the same settings ruins it, `scrape`'s facet, and `snakehook`'s pull with its stretched triangles left visible rather than hidden.

An entry SHALL show the surface-measured falloff on a shape where the straight-line one is wrong — a brush on one side of a narrow gap that does not reach across it.

An entry SHALL show a mask protecting half a region under one stroke, for a displacement verb and for `smooth`.

An entry SHALL show a stroke undone from its vertex deltas, with the render before and after and the equality stated in the output.

The examples SHALL print the quad and triangle counts before and after, because "topology never changes" is the claim and a number is how a reader checks it.

#### Scenario: The gallery runs
- **WHEN** `python examples/run_all.py` runs
- **THEN** the new entries run to completion and write their renders

#### Scenario: A reader can see the difference between two verbs
- **WHEN** a reader opens the clay and polish renders
- **THEN** the flat-topped strip and the surviving hard edge are visible, each beside the verb it is being distinguished from

### Requirement: An example exports quads from both sources
The gallery SHALL carry an example that quad-meshes an SDF document AND a voxel sculpt, writes OBJ, PLY and FBX, and prints for each the quad count actually produced against the count requested.

It SHALL also write GLB and print that it came out as triangles because glTF 2.0 has no quad primitive mode, so the one surprising outcome of this feature appears in the gallery output rather than in a bug report.

Its header comment SHALL state that the output is a REGULAR QUAD GRID DERIVED FROM A LATTICE and NOT field-aligned retopology — no edge loops following the form, nothing animation-ready — and SHALL say what a user should reach for a quad remesher for instead.

#### Scenario: The example runs and reports
- **WHEN** the quad export example runs
- **THEN** it exits zero, writes its OBJ, PLY, FBX and GLB under the gallery's output directory, and prints for each mesh the requested count and the count actually produced

#### Scenario: The honesty statement is in the example
- **WHEN** the example's header is read
- **THEN** it states that this is a lattice-derived quad grid rather than field-aligned retopology, and does not describe the result as animation-ready

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
