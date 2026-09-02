## ADDED Requirements

### Requirement: Prefix reuse reports where the agreement ends
When a tape is compiled by reusing another's prefix, the compiler SHALL report the point up to which the two tapes agree, so that a consumer other than the compiler can act on it.

Without this the reuse is invisible outside the compiler: the resulting tape has different bytes overall and nothing downstream can tell that most of them are the ones it already had. Reporting the boundary is what turns a cheaper compile into a cheaper upload.

The reported point SHALL be the compiler's actual agreement point, not an estimate — a boundary claimed further along than the tapes agree is a field that never existed, and it fails silently.

#### Scenario: The agreement point is where the prefix ended
- **WHEN** a document is compiled by reusing a prefix
- **THEN** the reported offsets are the lengths of the reused prefix, and the two tapes' sections are byte-identical below them

#### Scenario: A full compile reports no agreement
- **WHEN** a document is compiled without reusing a prefix
- **THEN** no agreement point is reported
