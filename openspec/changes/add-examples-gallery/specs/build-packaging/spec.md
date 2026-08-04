# build-packaging — examples gate

Delta for `add-examples-gallery`.

## ADDED Requirements

### Requirement: Examples CI job
CI SHALL build the Python module and run every script under `examples/`, failing the build on a non-zero exit from any of them.

#### Scenario: Examples job runs the gallery
- **WHEN** the examples job runs on a pull request
- **THEN** every example executes against the freshly built wheel and the job fails if any script raises
