# Tasks

- [x] 1.1 Reproduce #327 against a current build and against every prebuilt library in the tree
- [x] 1.2 Confirm the guard is unchanged at `v0.39.0`, the version #319 was measured against
- [x] 1.3 `read_transform` refuses a null position and a null rotation axis with separate messages
- [x] 1.4 The axis refusal names what to pass instead — an axis with angle 0
- [x] 1.5 `clay.h` states the requirement at `clay_layer_set_transform` and says why NULL is not "no rotation"
- [x] 1.6 `clay_document_set_layer_transform` gets the documentation it never had
- [x] 1.7 A regression test: the refusal, and the node's placement unchanged after it
- [x] 1.8 A regression test: the two messages differ and each names its own argument
- [x] 1.9 A regression test: `set_transform_nonuniform`, `set_layer_transform` and both mesh transforms refuse on the same terms
- [x] 1.10 Verify the message test FAILS with the split reverted, and that the revert compiles
