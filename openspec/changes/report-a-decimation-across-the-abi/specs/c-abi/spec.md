## ADDED Requirements

### Requirement: A decimation reports itself across the ABI

A mesh produced with `clay_mesh_params.decimate` SHALL carry the decimation
report, and the C ABI SHALL expose it: whether an edge of the RESULT carries
more than two incident triangles, whether the mesh handed to the simplifier did,
and how many simplifications were actually run.

**Validating the result is not the same answer.** A host holding a decimated
mesh can already discover that it carries a non-manifold edge. It cannot
discover whether decimation created that edge or was handed it, because the
input mesh no longer exists — and attribution is what the report is for. Nor can
it discover whether the retry ladder ran. Leaving those in C++ left every host
on this boundary able to see the defect and unable to place it.

The report SHALL be taken on every decimated call rather than on request. The
check has already been performed inside decimation itself, and a request flag on
the meshing descriptor would be a field a host compiled against an older layout
does not set — so the report would be absent exactly for the callers who most
need it.

**Whether the input was manifold SHALL be consulted only when the result is not.**
The input is examined only once the result is found pinched, because the answer
is wanted for attribution and nothing else, so a clean result reports a manifold
input without having looked and makes no claim about it.

A mesh that was NOT decimated SHALL be refused rather than answered with a clean
report. "Decimation ran and broke nothing" and "no decimation ran" are different
facts about an export, and a report that spells them the same way lets a host
believe a pass it never ran had succeeded.

**A rigid or affine motion of the mesh SHALL carry the report with it.** Such a
transform rewrites no index, so no edge changes incidence and the verdict remains
true of the result; dropping it would cost a host that re-orients an export
before writing it the only statement it had about that export's topology.

#### Scenario: A pinch decimation created is reported to a C host
- **WHEN** a host meshes a document through the C ABI with decimation requested, and the simplification pinches a surface that arrived 2-manifold
- **THEN** the report says the result is not manifold and that the input was, and a count of the simplifications that were run

#### Scenario: A pinch decimation was handed is not attributed to it
- **WHEN** the mesh given to decimation already carries an edge with more than two incident triangles
- **THEN** the report says the result is not manifold AND that the input was not, so the defect is attributed to what produced the input

#### Scenario: A mesh nobody decimated has nothing to report
- **WHEN** the decimation report is asked of a mesh that was meshed without decimation, built from triangles, loaded from a file, borrowed from a document layer or concatenated
- **THEN** the call is refused with an invalid-argument result rather than answered with a clean report

#### Scenario: A transformed mesh keeps its report
- **WHEN** a decimated mesh is transformed and the report is asked of the result
- **THEN** it is the report of the decimation that produced the mesh being transformed

#### Scenario: An older host is not overrun
- **WHEN** a host declares the struct_size of the layout it compiled against and asks for the report
- **THEN** exactly the fields that layout names are written, no byte past them is touched, and the struct_size it declared is the one it gets back
