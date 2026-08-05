// swift-tools-version:5.9
// SwiftPM wrapper (c-abi spec): the ClaySpace Xcode project consumes claycore
// as a package via the prebuilt xcframework produced by
// tools/build_xcframework.sh (run it before resolving this package).
//
// The wrapper contains no logic beyond packaging, plus one executable target
// that exists to be run rather than shipped: `swift run claycore-smoke` walks
// the C ABI from Swift exactly as an app would, so the package graph an app
// resolves is the same one that gets exercised. Without it `swift build` fails
// outright — a package whose only target is a binaryTarget has nothing
// buildable, so nothing verifies the manifest until an app tries to consume it.

import PackageDescription

let package = Package(
    name: "claycore",
    platforms: [.macOS(.v12), .iOS(.v16)],
    products: [
        .library(name: "claycore", targets: ["claycore"]),
        .executable(name: "claycore-smoke", targets: ["claycore-smoke"]),
    ],
    targets: [
        .binaryTarget(name: "claycore", path: "dist/claycore.xcframework"),
        .executableTarget(name: "claycore-smoke", dependencies: ["claycore"],
                          path: "tests/swift"),
    ]
)
