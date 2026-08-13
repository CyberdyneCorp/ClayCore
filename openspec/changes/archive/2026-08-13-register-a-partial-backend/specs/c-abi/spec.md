# c-abi — register a partial backend

Delta for `register-a-partial-backend` (#63, second half).

## ADDED Requirements

### Requirement: A host can tell a degraded backend from an absent one
The C ABI SHALL let a caller ask which operations a named backend can run, and SHALL let a caller read why a backend is absent or degraded. `clay_list_backends` answering `cpu` currently means either "no GPU backend is compiled into this build" or "one is, and it was discarded" — two states a host must act on differently and cannot distinguish.

`clay_backend_supports` SHALL report, for a named backend and a named operation, whether that operation is available. A backend that is not registered SHALL be `CLAY_ERROR_NOT_FOUND` rather than "supports nothing", because those are different answers: one is a backend that cannot do the thing, the other is a backend that is not there.

`clay_backend_diagnostic` SHALL return, by the size-query pattern `clay_list_backends` uses, a human-readable account of why a backend is unavailable or partial. It SHALL answer for a backend that is NOT registered — that is its main use — and SHALL yield an empty string for a backend that registered with every operation available, so "nothing to say" is expressible and is not an error.

The diagnostic SHALL carry the runtime's own words where it has any. A pipeline that failed to compile SHALL contribute the compiler's log rather than a summary of it: the log is what identifies the cause, and a host reporting a bug to us cannot recover what was already discarded.

Both calls SHALL be readable at any time and SHALL NOT require a document, a device or any prior call — a host decides which backend to ask for BEFORE it has built anything.

#### Scenario: A partial backend reports the operation it cannot run
- **WHEN** a backend is registered whose raycast pipeline is unavailable
- **THEN** `clay_backend_supports` reports the point and grid operations as available and raycast as unavailable, and `clay_backend_diagnostic` is non-empty and names the pipeline that failed

#### Scenario: An absent backend is not a silent one
- **WHEN** a backend is compiled in, fails to initialize, and is therefore not in `clay_list_backends`
- **THEN** `clay_backend_supports` for it is `CLAY_ERROR_NOT_FOUND` and `clay_backend_diagnostic` returns the reason it failed

#### Scenario: A backend that was never compiled in says so differently
- **WHEN** a backend name that this build does not contain is asked about
- **THEN** the diagnostic distinguishes "not compiled into this build" from "compiled in and failed", rather than answering the same way for both

#### Scenario: A fully working backend has nothing to say
- **WHEN** a registered backend has every operation available
- **THEN** `clay_backend_diagnostic` succeeds with an empty string, and `clay_backend_supports` reports every operation as available
