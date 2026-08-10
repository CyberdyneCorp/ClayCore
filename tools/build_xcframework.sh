#!/usr/bin/env bash
# Build dist/claycore.xcframework for SwiftPM consumption (c-abi spec):
# macOS (arm64), iOS device (arm64), iOS simulator (arm64).
#
# Every slice carries the CPU backend AND the Metal backend. Metal is the
# iPad app's production path, and CLAY_BACKEND_METAL decides which sources are
# COMPILED INTO libclaycore-*.a — so a consumer of a prebuilt static library
# cannot turn it on afterwards, whatever it links or sets in Xcode. Shipping it
# off meant every Apple host silently ran on the CPU: clay_list_backends
# returned "cpu" alone, and asking for "metal" failed because nothing had
# registered it.
#
# Consumers must link Metal and Foundation. Package.swift declares both for the
# SwiftPM path; a plain C or C++ harness passes -framework Metal
# -framework Foundation itself.
#
# Backend options are passed EXPLICITLY on every configure, including the ones
# left off. CMake caches option() values, and these build directories are not
# deleted between runs, so an inherited cache would otherwise decide what
# ships — which has already produced one silently wrong A/B measurement, where
# a run intended as CPU-only was still Metal.
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
    local build_dir="$ROOT/build/xc-$name"
    cmake -S "$ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCLAY_BUILD_TESTS=OFF \
        -DCLAY_BUILD_BENCHMARKS=OFF \
        -DCLAY_BACKEND_METAL=ON \
        -DCLAY_BACKEND_CUDA=OFF \
        -DCLAY_BACKEND_OPENCL=OFF \
        -DCLAY_BACKEND_VULKAN=OFF \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        "$@" > /dev/null
    cmake --build "$build_dir" --target claycore -j > /dev/null

    # The metallib is embedded, so an empty or missing one is a CPU-only slice
    # that still links and still passes a build. Fail here rather than ship it.
    local metallib="$build_dir/clay_kernels.metallib"
    if [[ ! -s "$metallib" ]]; then
        echo "no Metal library was built for slice $name ($metallib)" >&2
        exit 1
    fi
    # merge claycore with its static dependencies into one library
    libtool -static -o "$STAGE/libclaycore-$name.a" \
        "$build_dir/libclaycore.a" \
        "$build_dir/_deps/meshoptimizer-build/libmeshoptimizer.a"

    # And that the merged archive still carries it. This is the check that is
    # independent of any device: the Swift smoke test asserts the backend
    # REGISTERS, which needs a Metal device to be present, while this asserts
    # the artifact SHIPS it, which is what the build controls and what was
    # actually wrong.
    if ! nm "$STAGE/libclaycore-$name.a" 2>/dev/null | grep -q "clay_metallib"; then
        echo "slice $name has no embedded Metal library — it would ship CPU-only" >&2
        exit 1
    fi
    echo "built slice: $name (metal: $(wc -c < "$metallib") bytes)"
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
