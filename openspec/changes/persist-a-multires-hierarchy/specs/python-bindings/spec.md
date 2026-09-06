# python-bindings — pyclay reaches a document's hierarchies

Delta for `persist-a-multires-hierarchy`.

## ADDED Requirements

### Requirement: pyclay reaches a document's hierarchies
pyclay SHALL expose whether a layer carries a multiresolution hierarchy and SHALL expose the hierarchy a load produced, on the same terms the C ABI does. `tools/check_binding_parity.py` fails otherwise, and a capability reachable from C and not from Python is one the examples and the gallery cannot exercise — which is how an entry point ends up called by nothing.

The Python surface SHALL follow the ownership the C ABI states rather than inventing a second one: the document holds the hierarchy, and a Python object referring to it does not remove it from the document when it is collected.

#### Scenario: A saved hierarchy round trips through pyclay
- **WHEN** a document holding a hierarchy is saved and reloaded from Python, and its layers are inspected
- **THEN** the hierarchy's layer reports that it carries one, and its level count matches what was saved

#### Scenario: Collecting the Python object keeps the document's hierarchy
- **WHEN** a Python reference to a loaded hierarchy is dropped and the document is saved
- **THEN** the hierarchy is still written
