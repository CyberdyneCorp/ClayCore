#!/usr/bin/env bash
# Build dist/claycore.xcframework for SwiftPM consumption (c-abi spec):
# macOS (arm64), iOS device (arm64), iOS simulator (arm64).
#
# The xcframework bundles the CPU backend (portable everywhere). The Metal
# backend is wired per-app during Xcode integration (the app links
# Metal.framework and enables CLAY_BACKEND_METAL in its build); the C ABI
# and results are identical either way — backends change speed, not values.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
DIST="$ROOT/dist"
STAGE="$ROOT/build/xcframework"
rm -rf "$DIST/claycore.xcframework" "$STAGE"
mkdir -p "$DIST" "$STAGE/headers"

cp bindings/c/clay.h "$STAGE/headers/"
cat > "$STAGE/headers/module.modulemap" <<'EOF'
module claycore {
    header "clay.h"
    export *
}
EOF

build_slice() {
    local name="$1"; shift
    local build_dir="$ROOT/build/xc-$name"
    cmake -S "$ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCLAY_BUILD_TESTS=OFF \
        -DCLAY_BUILD_BENCHMARKS=OFF \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        "$@" > /dev/null
    cmake --build "$build_dir" --target claycore -j > /dev/null
    # merge claycore with its static dependencies into one library
    libtool -static -o "$STAGE/libclaycore-$name.a" \
        "$build_dir/libclaycore.a" \
        "$build_dir/_deps/meshoptimizer-build/libmeshoptimizer.a"
    echo "built slice: $name"
}

# Without an explicit deployment target the macOS slice inherits the host's
# SDK version, so a library built on a new machine refuses to link into an app
# targeting anything older. Keep this in step with Package.swift's platforms.
build_slice macos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0

build_slice ios \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0

build_slice ios-sim \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0

xcodebuild -create-xcframework \
    -library "$STAGE/libclaycore-macos.a" -headers "$STAGE/headers" \
    -library "$STAGE/libclaycore-ios.a" -headers "$STAGE/headers" \
    -library "$STAGE/libclaycore-ios-sim.a" -headers "$STAGE/headers" \
    -output "$DIST/claycore.xcframework"

echo "dist/claycore.xcframework ready"
