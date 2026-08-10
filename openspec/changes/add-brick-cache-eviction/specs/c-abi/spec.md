# c-abi — releasing brick memory from a packaged host

Delta for `add-brick-cache-eviction`.

## ADDED Requirements

### Requirement: Brick memory can be released across the C ABI
The C ABI is the only surface a packaged consumer has, so the cache's release path SHALL be reachable through it: trimming to a target number of bytes, and reporting the usage reached and the number of bricks dropped.

The surface SHALL mirror `brick::BrickCache` rather than inventing a second policy, and SHALL NOT publish an eviction loop, timer or threshold — a host asks, on its own schedule, for its own reasons (a platform memory warning being the expected one).

Whatever ordering decides which bricks go SHALL be stated in the ABI's documentation, and where the ordering depends on information only the host has, the host SHALL be able to supply it.

Statistics SHALL make visible the growth the budget does not bound, so a host can tell a cache that is holding data from one that is holding bookkeeping.

#### Scenario: A host answers a memory warning
- **WHEN** a host trims the cache to a target on receiving a platform memory warning
- **THEN** the call reports the usage reached and the bricks dropped, and every remaining brick is still readable and correct

#### Scenario: Trimming does not disturb the dirty set
- **WHEN** a trim happens while bricks are dirty and requests are outstanding
- **THEN** outstanding requests are still resolvable — accepted, or refused as stale by the ordinary generation rule — and no submitted brick lands in a slot it does not own

#### Scenario: Statistics show what the budget does not
- **WHEN** a host reads the cache statistics
- **THEN** it can see both the payload usage the budget bounds and the tracked-key growth it does not
