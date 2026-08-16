# c-abi — closing the mesh brush vocabulary

Delta for `close-mesh-brush-vocabulary`.

## ADDED Requirements

### Requirement: The new mesh verbs and alphas reach the C ABI additively
The C API SHALL expose the added verbs as new enumerators and the alpha as new settings fields, so a caller compiled against the previous header behaves exactly as before.

Alpha samples SHALL be COPIED or borrowed only for the duration of the call, and the contract SHALL say which.

#### Scenario: An existing caller is unaffected
- **WHEN** a caller sets no alpha and uses no new verb
- **THEN** every existing call behaves exactly as before

#### Scenario: A malformed alpha is refused rather than applied
- **WHEN** a caller passes a null sample pointer or a non-positive dimension
- **THEN** the call is refused and the mesh is unchanged
