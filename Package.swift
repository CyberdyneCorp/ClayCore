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
        // STATIC, and it must stay that way (#112).
        //
        // Left automatic, Xcode 26 builds this product as a DYNAMIC
        // PackageProduct framework in Debug. That framework links
        // libclaycore-*.a, but ClayCoreLink is one empty Swift file that
        // references no archive member, so the linker pulls ZERO objects: the
        // framework exports no clay_* symbols and every consuming app fails to
        // link with every symbol undefined. The archive is fine throughout —
        // `nm -gU` shows them all — which is what makes the failure so hard to
        // read from the app side.
        //
        // At 0.25.0 the product WAS the binaryTarget, which Xcode always links
        // directly into the client; vending ClayCoreLink instead (0.26, to
        // carry the Metal/Foundation linker settings a binaryTarget cannot)
        // is what opened this. Declaring the product static removes the
        // dynamic-framework path entirely rather than relying on which
        // configuration Xcode chose.
        //
        // `swift run claycore-smoke` CANNOT catch a regression here: an
        // executable's direct symbol references pull the archive members
        // whatever the product linkage, which is exactly the reference the
        // empty wrapper lacks. tools/check_swift_package.py gates it instead.
        .library(name: "claycore", type: .static, targets: ["ClayCoreLink"]),
        .executable(name: "claycore-smoke", targets: ["claycore-smoke"]),
    ],
    targets: [
        .binaryTarget(name: "claycoreBinary", path: "dist/claycore.xcframework"),
        // Metal and Foundation are what the Metal backend inside the archive
        // needs, and a binaryTarget cannot carry linker settings. Declaring
        // them on the smoke executable alone would fix `swift build` here and
        // leave every consuming app to discover the undefined symbols for
        // itself, so they live on a target the PRODUCT vends instead: depend
        // on `claycore` and the frameworks come with it.
        //
        // ClayCoreLink carries no code, only that knowledge. The C API is
        // still `import claycore`, from the module map inside the xcframework
        // — this target adds no module of its own worth importing.
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
