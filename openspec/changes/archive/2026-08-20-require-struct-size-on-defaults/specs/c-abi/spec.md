# c-abi

## ADDED Requirements

### Requirement: A defaults call is an output descriptor
An entry point that fills a descriptor with engine defaults SHALL be treated as an output descriptor like any other: the caller SHALL set `struct_size` before the call, and the library SHALL bound its fill to that size. Setting `struct_size` on the caller's behalf SHALL NOT be offered as a convenience, because a descriptor the caller does not measure is one the library cannot bound — the fill then uses the library's own `sizeof`, which overruns any host built against a layout that has since grown.

A call whose descriptor declares no size, or a size below the descriptor's original layout, SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and SHALL leave the caller's memory untouched. Refusal SHALL be understood as the best available outcome rather than a complete one: a host compiled before the requirement declares nothing, so it cannot be served correctly, and the choice is only between refusing it and corrupting it.

This SHALL apply however the descriptor is filled, including where an entry point fills it by delegating to another entry point rather than assigning it directly.

#### Scenario: A defaults call without a declared size
- **WHEN** a caller passes a descriptor whose `struct_size` is zero, or below the descriptor's original layout, to a defaults-style call
- **THEN** the call is refused with `CLAY_ERROR_INVALID_ARGUMENT` and no field of the caller's struct is written

#### Scenario: A defaults call from an older header
- **WHEN** a host declares the layout it was compiled against and that layout is shorter than the current one
- **THEN** the defaults are filled into exactly the bytes declared, nothing past them is touched, and the declared size is returned

#### Scenario: A defaults call from the current header
- **WHEN** a caller declares the current layout
- **THEN** every field is filled, including those appended after the original layout

### Requirement: The output-fill rule is enforced mechanically
The ABI hygiene gate SHALL verify that every entry point taking a versioned descriptor by mutable pointer fills it through a bounded write, rather than relying on review to notice. Searching the implementation for a single spelling SHALL NOT be considered sufficient: one site filled its output descriptor by delegating to another entry point, matched no such search, and was missed by a sweep that believed itself complete.

#### Scenario: An unbounded output fill is added
- **WHEN** an entry point writes a versioned output descriptor without a bounded fill
- **THEN** the C ABI hygiene check fails naming the entry point and the descriptor
