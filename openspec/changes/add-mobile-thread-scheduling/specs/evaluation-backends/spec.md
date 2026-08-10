# evaluation-backends — workers the OS and the host can both see

Delta for `add-mobile-thread-scheduling`.

## ADDED Requirements

### Requirement: Workers declare their scheduling class
On platforms with a quality-of-service or thread-priority concept, the CPU backend's workers SHALL declare one rather than inheriting the runtime's default. On Apple platforms this SHALL be a QoS class.

The default SHALL suit interactive work — a caller is waiting on a brush dab — and SHALL be overridable by the host. Where a platform has no equivalent concept, the absence SHALL be a stated no-op rather than an unnoticed gap.

#### Scenario: A worker runs at a declared class
- **WHEN** the pool dispatches a batch on a platform with a QoS concept
- **THEN** its workers run at the configured class rather than the platform default

### Requirement: The pool is sized for cores that are not interchangeable
The worker count SHALL be derived from the number of PERFORMANCE cores where the platform distinguishes them, not from the count of every logical core. A mobile SoC pairs fast cores with efficiency cores by design, and a worker per efficiency core oversubscribes a device that is also drawing the user's interface.

Where the platform does not distinguish core types, the fallback SHALL be stated rather than implied.

The host SHALL be able to override the count, including setting it to zero, in which case batches SHALL be evaluated serially on the calling thread.

#### Scenario: A host asks for no library threads
- **WHEN** the worker count is configured to zero
- **THEN** every batch is evaluated on the calling thread, and the results are identical to a threaded run

#### Scenario: A host sizes the pool
- **WHEN** the worker count is configured before the first evaluation
- **THEN** the pool uses that count

### Requirement: A thread with no work sleeps rather than spins
A thread that has no chunk left to claim SHALL wait without consuming CPU until the batch completes. Spinning on a yield loop costs clock and battery on a thermally limited device, and the thread most likely to be spinning is the one the user is waiting on.

#### Scenario: The calling thread stops burning a core
- **WHEN** a batch is dispatched whose chunks finish at very different times
- **THEN** the threads that finish early are not executing while they wait

#### Scenario: Completion is still exact
- **WHEN** any batch completes
- **THEN** every element has been computed exactly once and no worker is still inside the batch's function
