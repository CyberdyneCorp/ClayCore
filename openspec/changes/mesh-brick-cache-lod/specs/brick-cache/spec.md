# brick-cache — sample and enumerate a level

Delta for `mesh-brick-cache-lod` (#93).

## ADDED Requirements

### Requirement: A level can be sampled and enumerated, not only read back
The cache SHALL answer a decoded sample AT A LEVEL, and SHALL enumerate the keys a level stores. Before this the cache could sample and enumerate the full-resolution level only, and a mip was reachable through whole-brick readback alone — which is why nothing but a readback consumer could use one.

A level that holds no brick for a key SHALL answer the outside band value, as a never-evaluated brick does, so that a lattice walk over any level is total and a consumer needs no special case at the edge of the built region. The level-0 answers SHALL be exactly what the existing sample and enumeration return.

The count of built mips SHALL be available in constant time, so a consumer can ask whether a level exists at all without enumerating it.

#### Scenario: A level's enumeration is what it stores
- **WHEN** some coarse blocks have their mips built and others do not
- **THEN** enumerating level 1 yields exactly the coarse keys whose mip is valid, and enumerating a level above 1 yields nothing

#### Scenario: Sampling outside the built region is defined
- **WHEN** a level is sampled at a key it holds no brick for
- **THEN** the answer is the outside band value rather than an error or an undefined read
