## ADDED Requirements

### Requirement: The topology cache is reachable across the C ABI

A host SHALL be able to read the document's topology cache occupancy and effect
through the C ABI, and SHALL be able to release what no live sculptor is
holding.

The stats call SHALL take a `struct_size`-prefixed descriptor and SHALL report
at least entries, bytes, hits, misses, evictions, and the cumulative time spent
building and verifying entries. The hit and miss counts are what tells a host
integrator that sharing is happening at all: a cache that never hits and a cache
that is absent are otherwise indistinguishable from outside, and this library
has shipped features before whose whole defect was that the engine could do it
and the host could not see it.

The trim call SHALL report the bytes it released, and SHALL release nothing a
live sculptor is holding.

The cache's bytes SHALL appear in `clay_memory_report` as their own line inside
the rebuildable roll-up, appended after the existing fields so that every field
a shipped caller compiled against stays at its offset.

#### Scenario: A host reads the cache's effect
- **WHEN** a host creates two sculptors over one unchanged mesh layer and asks for the cache stats
- **THEN** the stats report one entry, one miss and one hit

#### Scenario: A host releases what nothing holds
- **WHEN** a host destroys its sculptors and trims the topology cache
- **THEN** the bytes released are reported and the cache reports no entries

#### Scenario: A trim during a session releases nothing
- **WHEN** a host trims the topology cache while a sculptor exists over the cached layer
- **THEN** nothing is released and the sculptor continues to stamp

#### Scenario: The bytes are accounted for
- **WHEN** a host reads the document memory report with entries in the topology cache
- **THEN** the cache's bytes appear on their own line and inside the rebuildable total
