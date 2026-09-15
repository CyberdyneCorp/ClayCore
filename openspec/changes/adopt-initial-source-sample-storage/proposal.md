# Adopt initial source sample storage

## Why

Smooth/Relax preview priming fills a temporary sample block and copies it into a second allocation owned by the working volume. For an initially empty volume, the filled block can become that storage directly. Issue #531 still requires substantial latency reductions; this increment removes a measured allocation/copy cost without changing sampling.

## What changes

Transfer the first filled block into empty volume storage. Keep existing sample bits, callback order, incremental append behavior, brick indices, Lipschitz bounds and dirty-coordinate reporting. Add an allocation regression and explicit full/partial/repeated materialization coverage.

## Scope

Private field storage only. No public API, serialization or application pin changes. The full 16 ms goal remains open; allocation evidence does not establish application latency.
