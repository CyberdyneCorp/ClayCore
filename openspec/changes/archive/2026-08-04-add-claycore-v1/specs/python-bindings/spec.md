# python-bindings — pyclay (nanobind, numpy-native)

Delta for `add-claycore-v1`. Python is both a first-class consumer and the harness used to exercise/test the library (golden-scene corpus authoring, property tests, batch validation).

## ADDED Requirements

### Requirement: pyclay module
The library SHALL ship a nanobind extension module `pyclay` exposing: document/layer construction (`Document`, `add_sdf_layer`, `add_voxel_layer`), the full edit vocabulary (primitives, ops, blends, transforms, deformers, mirrors, strokes) with Pythonic parameter names, field evaluation (`eval`, `gradients`), meshing with resolution/decimation/backend selection, mesh predicates (`is_watertight()` etc.), and save/load of `.clayspace` plus mesh export (OBJ/FBX/PLY/glTF).

#### Scenario: Authoring flow
- **WHEN** a script builds a layer with `body.add(clay.Sphere(r=1.0), blend=clay.Smooth(0.2), color="#38a6cf")`, meshes it, and saves `body.clayspace`
- **THEN** the resulting file opens in any claycore consumer (including the iPad app) and evaluates identically

### Requirement: numpy-native data exchange
All batch APIs SHALL accept and return numpy arrays without copies where layout permits: points as `(N,3) float32` in, distances `(N,) float32` out, gradients `(N,3) float32`, colors `(N,3)/(N,4)`, mesh buffers as arrays. Evaluation SHALL release the GIL.

#### Scenario: Zero-copy evaluation
- **WHEN** a C-contiguous `(N,3) float32` array is passed to `eval`
- **THEN** no input copy is made, the GIL is released during evaluation, and a `(N,) float32` array is returned

### Requirement: Backend selection from Python
`pyclay` SHALL expose backend enumeration and per-call backend selection (`backend="cpu" | "metal" | "cuda" | "opencl"`), defaulting to CPU, with a clear error when an unavailable backend is requested.

#### Scenario: Unavailable backend
- **WHEN** `backend="cuda"` is requested on a machine without CUDA
- **THEN** a Python exception names the backend as unregistered and lists available backends

### Requirement: Wheels
`pyclay` SHALL be packaged with scikit-build-core and built via cibuildwheel for macOS (arm64, x86-64), Linux (manylinux x86-64/aarch64), and Windows (x86-64), always containing the CPU backend and containing GPU backends where the build platform provides them.

#### Scenario: CPU wheel just works
- **WHEN** `pip install pyclay` runs on a clean supported machine and the quickstart script executes
- **THEN** evaluation, meshing, and file export succeed with no GPU or extra system dependencies

### Requirement: Python as test harness
The golden-scene test corpus and property-test scenarios SHALL be authorable in Python against `pyclay`, and CI SHALL run these Python-driven suites against the same library binary the native tests use.

#### Scenario: Golden corpus from Python
- **WHEN** CI runs the Python corpus generator and the meshing gate
- **THEN** every generated scene meshes watertight/manifold and matches stored golden hashes (or intentional-change review is triggered)
