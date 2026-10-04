## ADDED Requirements

### Requirement: Minor 20 carries an item's own mirror axes
Scene and container minor 20 SHALL append one byte per node record: the item's own mirror axes, or the inherit value. It SHALL be written and read only at minor 20 and above. A document read at an older minor SHALL load every item inheriting.

A document whose items all inherit SHALL write, at minor 19, exactly the bytes minor 19 always wrote. A document holding any item with its own axes SHALL NOT be written below minor 20. The writer SHALL refuse it rather than drop the byte, and `layer_blocking_minor` SHALL name the first layer holding such an item.

#### Scenario: Own axes round-trip
- **WHEN** a document holding an item with its own axes X is written and read at minor 20
- **THEN** the item carries X again, evaluates with its twin, and re-writing it reproduces the stream

#### Scenario: An inheriting document still writes minor 19's bytes
- **WHEN** a document whose items all inherit is written at 19, read back, and written at 19 again
- **THEN** the two streams are identical and the write is not refused
