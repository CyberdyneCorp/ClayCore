# append-into-a-group

A dab added inside a group misses the append fast path and recompiles the whole tape: 90x per dab at a thousand items, measured on the reference iPad
