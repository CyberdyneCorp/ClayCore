# c-abi — an edit's influence covers every instance of the node

Delta for `bound-an-edit-across-instances`.

## ADDED Requirements

### Requirement: An influence bound covers every place the node is compiled
A bound reported for a NODE SHALL cover every place that node can change the field, which is once per layer sharing its content and not only the layer the caller named. Instancing a layer shares one edit list between layers with different transforms, so a single node is compiled once per instancing layer and an edit moves every copy.

The bound a host DIRTIES BY and the bound a host is TOLD SHALL be the same union, since a host that dirties by what it was told and is left with stale geometry has no way to discover the disagreement.

The per-layer bound SHALL remain available and unchanged for the compiler, which compiles one tape per layer and wants the box for the layer it is compiling.

#### Scenario: Moving a node changes nothing outside its declared box
- **GIVEN** any document and any visible item in it
- **WHEN** the item is moved and the band-clamped field is compared before and after
- **THEN** every point outside the union of the bound before and the bound after, dilated by the band and the chain pad, evaluates to exactly what it did

#### Scenario: An instanced layer's other copy is inside the box
- **GIVEN** a layer instanced into a second layer at a different transform
- **WHEN** a node of the shared content is moved
- **THEN** the second layer's copy lies inside the declared box, so a host dirtying by it refills that copy too

#### Scenario: A document with no instancing is unaffected
- **GIVEN** a document in which no layer shares content with another
- **WHEN** a node's influence bound is read
- **THEN** it is the bound the single layer reports
