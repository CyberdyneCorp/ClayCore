# c-abi — the host can see and size the library's threads

Delta for `add-mobile-thread-scheduling`.

## ADDED Requirements

### Requirement: Worker scheduling is configurable across the C ABI
The C ABI is the only surface a packaged consumer has, so the library's worker pool SHALL be configurable and inspectable through it: at minimum the worker count and, on platforms that have one, the scheduling class.

Configuration SHALL be a versioned descriptor, consistent with every other configuration surface in this ABI, and SHALL be applicable before any evaluation has happened. Whether it may be changed afterwards SHALL be stated rather than left to chance.

A consumer that configures nothing SHALL get the default behaviour with no change in results.

This does not publish a refill loop, a time budget or an ordering policy — the consumer still owns queues and scheduling. It publishes only what the library itself spawns, because a host cannot own threading it cannot see.

#### Scenario: A host reads what the library spawned
- **WHEN** a host queries the worker configuration
- **THEN** it learns the worker count actually in use and the scheduling class, whether or not it configured them

#### Scenario: Configuring nothing changes nothing
- **WHEN** a consumer never touches the configuration
- **THEN** evaluation behaves as it did before this change

#### Scenario: An invalid configuration is refused, not clamped silently
- **WHEN** a configuration outside the supported range is submitted
- **THEN** an error identifying the field is returned and the previous configuration remains in force
