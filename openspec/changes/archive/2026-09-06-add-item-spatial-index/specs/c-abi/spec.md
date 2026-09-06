# c-abi — a request batch is evaluated as a batch

Delta for `add-item-spatial-index`.

## ADDED Requirements

### Requirement: A brick request batch is evaluated as a batch
`clay_brick_cache_eval_requests` SHALL evaluate the requests it is given as one batch rather than as a serial loop on the calling thread, dividing them across the same worker pool every other batch entry point uses.

The requests in a batch are independent by construction — each writes its own stride of the output buffer — so the order in which they are evaluated SHALL NOT be observable in the results.

A batch SHALL share one compiled document across its requests: the per-request cull is a query against shared state, not a reason to rebuild that state per request.

#### Scenario: A batch is order-independent
- **WHEN** the same request batch is evaluated twice
- **THEN** the output buffer is bit-identical both times, whatever order the requests were completed in

#### Scenario: A dab's cost is dominated by evaluation, not by culling
- **WHEN** a dab's worth of requests is evaluated against a 10 000-item document
- **THEN** the time spent deciding which items each brick needs is a minority of the call

#### Scenario: A single request is not made slower
- **WHEN** a batch of one request is evaluated
- **THEN** it costs no more than it did before this change
