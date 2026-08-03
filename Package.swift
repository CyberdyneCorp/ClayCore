// swift-tools-version:5.9
// SwiftPM wrapper (c-abi spec): the ClaySpace Xcode project consumes
// claycore as a package via the prebuilt xcframework produced by
// tools/build_xcframework.sh (run it before resolving this package).
// The wrapper contains no logic beyond packaging.

import PackageDescription

let package = Package(
    name: "claycore",
    products: [
        .library(name: "claycore", targets: ["claycore"])
    ],
    targets: [
        .binaryTarget(name: "claycore", path: "dist/claycore.xcframework")
    ]
)
