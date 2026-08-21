# brick-cache — a ceiling you can come back down from

Delta for `add-brick-cache-eviction`.

## MODIFIED Requirements

### Requirement: Deterministic memory ceiling
The cache SHALL enforce a configurable memory budget suitable for mobile: allocation beyond the budget SHALL fail predictably (error code / eviction policy), never abort or throw across the ABI, and the current usage SHALL be queryable.

The cache SHALL additionally be able to RELEASE memory on request: dropping the payloads of bricks it is holding, and reporting how much it freed. A budget that can only be reached and never backed away from leaves a host with no answer to a platform memory warning except destroying the cache, which is the most expensive operation available at the worst moment to take it.

An evicted brick SHALL become a brick that has not been evaluated — a state the cache already has — so that looking at it again re-enters the ordinary dirty / request / submit cycle and produces the same data it held before.

The growth that the budget does NOT bound — the per-key bookkeeping, which grows with how much space has ever been marked dirty — SHALL either be bounded by the same request or be queryable as a number a host can act on. Unbounded growth that no query reveals is not an acceptable resting state.

Eviction SHALL be something the host asks for, not a loop the cache runs behind it: the consumer owns scheduling here as everywhere else in this capability.

#### Scenario: Budget exceeded
- **WHEN** an evaluation would allocate bricks beyond the configured budget
- **THEN** the request returns a budget-exceeded result identifying the shortfall, and existing brick data remains valid

#### Scenario: Trim to a target
- **WHEN** a host asks the cache to reduce its usage to a target number of bytes
- **THEN** the cache releases brick payloads until it is at or below the target if it can, and reports the usage it actually reached

#### Scenario: An evicted brick comes back identical
- **WHEN** a brick is evicted and then re-dirtied, re-evaluated and resubmitted from an unchanged document
- **THEN** its data is bit-identical to what it held before eviction

#### Scenario: Recovering from a refused submit
- **WHEN** a submit is refused for budget and the host then frees space and resubmits the same request
- **THEN** the resubmission is accepted, subject to the ordinary generation check

#### Scenario: Eviction is never implicit
- **WHEN** a host never asks the cache to release anything
- **THEN** the cache holds exactly what it holds today and evicts nothing on its own
