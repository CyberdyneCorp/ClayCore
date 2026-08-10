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
        .library(name: "claycore", targets: ["ClayCoreLink"]),
        .executable(name: "claycore-smoke", targets: ["claycore-smoke"]),
    ],
    targets: [
        .binaryTarget(name: "claycoreBinary", path: "dist/claycore.xcframework"),
        // Every xcframework slice bundles the Metal backend, and a
        // static-library xcframework records no dependency on the frameworks
        // its objects reference. This target holds that knowledge so a
        // consumer does not have to: depend on it and Metal comes linked.
        // The C API is still `import claycore`, from the module map inside
        // the xcframework — this target adds no module of its own worth
        // importing.
        .target(name: "ClayCoreLink", dependencies: ["claycoreBinary"],
                path: "bindings/swift/ClayCoreLink",
                linkerSettings: [
                    .linkedFramework("Metal"),
                    .linkedFramework("Foundation"),
                ]),
        .executableTarget(name: "claycore-smoke", dependencies: ["ClayCoreLink"],
                          path: "tests/swift"),
        // The device harness is NOT a target here. XCTest has no hostless mode
        // on a device destination and SwiftPM cannot declare a test host, so
        // the on-device tests live in a generated Xcode project instead —
        // tests/device/project.yml, driven by tools/run_device_bench.sh.
    ]
)
