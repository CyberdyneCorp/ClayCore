## MODIFIED Requirements

### Requirement: Versioning
The C ABI and Python API SHALL follow SemVer; kernel headers may evolve freely within a major. The document format version is independent (backward-open, forward-refuse per `file-io`). GPU backend availability SHALL never change results — only speed — enforced by the parity suite as a release gate.

The release checklist SHALL NAME the backends its parity row compared, and SHALL name the hardware backends that were not built, so that a green row on a build containing none of them cannot be read as coverage of them. A parity run that does not report which backends it compared SHALL FAIL the row: an absent backend passes every assertion it never reaches, so silence about what was compared is not a result.

Where a backend's parity cannot be gated by any runner the project has, the release SHALL still state its position on it. For each such gate — CUDA device parity, the nvcc build of the backend including its architecture auto-detection, OpenCL parity on a real device, and Vulkan parity on real silicon — the repository SHALL carry a record naming the commit it speaks for, and saying either that the gate was RUN, with the evidence and the hardware it ran on, or that it was WAIVED, with a reason. A recorded waiver SHALL satisfy the gate: the requirement is that the release says which of them it ran and which it is shipping without, not that hardware exists.

A record SHALL expire when what it certifies has changed since the commit it names: a change to the kernel sources, the backends that compile them, the generators that derive the OpenCL and GLSL dialects, or the CUDA architecture selection expires every gate, and a change to the parity corpus expires the gates that are parity experiments. The checklist SHALL fail any gate whose record is missing, unexplained, or expired.

#### Scenario: Release checklist enforced
- **WHEN** a release tag is cut
- **THEN** CI verifies ABI version bump correctness (no symbol/layout break on minor), wheel builds, and parity-suite pass on all registered backends

#### Scenario: The parity row names what it compared
- **WHEN** the release checklist runs against a build with no GPU backend compiled in
- **THEN** the parity row names the backends it compared and names the hardware backends it did not, rather than reporting a count that is identical with and without a GPU present

#### Scenario: A parity run that reports no backends fails
- **WHEN** the parity run exits zero but prints no line naming the backends it compared
- **THEN** the parity row FAILS, because what it covered is unknown

#### Scenario: An unanswered hardware gate fails the release
- **WHEN** the release checklist runs and one of the four manual hardware gates has no record, has a record with no evidence or reason, or has a record older than the last change to the kernels or — for a parity gate — to the parity corpus
- **THEN** that gate's row FAILS and names which of the two it is, so the release either runs the gate on hardware or records a waiver against this tree

#### Scenario: A waiver with a reason satisfies a hardware gate
- **WHEN** a manual hardware gate is recorded as waived, against the current tree, with a reason
- **THEN** that gate's row passes and the release states, in the checklist output, what it is shipping without
