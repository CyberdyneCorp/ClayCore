// Deliberately empty.
//
// This target carries no code — it carries LINK-TIME KNOWLEDGE. The
// xcframework's slices are static libraries, and a static-library xcframework
// records no dependency on the frameworks its objects reference. Since every
// slice now bundles the Metal backend (tools/build_xcframework.sh), consuming
// the binary target alone fails to link with undefined `MTL*` and `NS*`
// symbols, and every consumer would have to discover that for itself.
//
// Depending on this target instead brings Metal and Foundation with it. The C
// API is still `import claycore` — the clang module comes from the module map
// inside the xcframework, and this target is only in the graph to hold the
// linker settings in Package.swift.
