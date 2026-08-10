#!/usr/bin/env bash
# Build dist/claycore.xcframework for SwiftPM consumption (c-abi spec):
# macOS (arm64), iOS device (arm64), iOS simulator (arm64).
#
# Every slice bundles the CPU backend (portable everywhere) AND the Metal
# backend, which is the production path on iPad. Each slice's embedded metallib
# is compiled for that slice's own SDK — see the SDK derivation in
# CMakeLists.txt, and note that AIR built for the wrong platform loads on no
# device while looking, at the ABI, exactly like a backend that was never
# enabled. The C ABI and results are identical either way; backends change
# speed, not values.
#
# A consumer must link Metal.framework and Foundation.framework, because these
# slices are STATIC libraries and an xcframework of static libraries carries no
# link-time dependency information of its own.
#
# Each slice's Headers also carries the kernel dialect under clay/kernel/, so
# an app drawing its own GPU preview compiles ClayCore's distance functions
# rather than copying them (build-packaging: published kernels artifact). The
# module map still declares only clay.h — the kernel headers are shader source
# for the Metal compiler, not part of the Swift module.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
DIST="$ROOT/dist"
STAGE="$ROOT/build/xcframework"
rm -rf "$DIST/claycore.xcframework" "$STAGE"
mkdir -p "$DIST" "$STAGE/headers"

cp bindings/c/clay.h "$STAGE/headers/"
mkdir -p "$STAGE/headers/clay/kernel"
cp include/clay/kernel/*.h "$STAGE/headers/clay/kernel/"
cat > "$STAGE/headers/module.modulemap" <<'EOF'
module claycore {
    header "clay.h"
    export *
}
EOF

build_slice() {
    local name="$1"; shift
    local expect_platform="$1"; shift
    local build_dir="$ROOT/build/xc-$name"
    cmake -S "$ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCLAY_BUILD_TESTS=OFF \
        -DCLAY_BUILD_BENCHMARKS=OFF \
        -DCLAY_BACKEND_METAL=ON \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        "$@" > /dev/null
    cmake --build "$build_dir" --target claycore -j > /dev/null

    # A metallib built for the wrong platform links fine, ships fine, and then
    # fails to load on the device — where it is indistinguishable from a
    # backend that was never enabled. The platform is recorded in the file, so
    # check it here rather than discovering it on an iPad.
    local got
    got=$(xcrun metal-readobj --file-headers "$build_dir/clay_kernels.metallib" \
          | awk '/Platform:/ {print $2}')
    if [ "$got" != "$expect_platform" ]; then
        echo "slice $name: metallib is $got, expected $expect_platform" >&2
        exit 1
    fi

    # merge claycore with its static dependencies into one library
    libtool -static -o "$STAGE/libclaycore-$name.a" \
        "$build_dir/libclaycore.a" \
        "$build_dir/_deps/meshoptimizer-build/libmeshoptimizer.a"
    echo "built slice: $name ($got)"
}

# Without an explicit deployment target the macOS slice inherits the host's
# SDK version, so a library built on a new machine refuses to link into an app
# targeting anything older. Keep this in step with Package.swift's platforms.
build_slice macos METALLIB_PLATFORM_MACOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0

build_slice ios METALLIB_PLATFORM_IOS \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0

build_slice ios-sim METALLIB_PLATFORM_IOS_SIMULATOR \
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
