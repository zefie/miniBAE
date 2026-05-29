#!/usr/bin/env bash
# Build NeoBAE native library for one or more targets and drop the result
# under native/build/<os>/<arch>/ where Gradle picks it up.
#
# Usage:
#   ./build_native.sh                        # build for the current host
#   ./build_native.sh linux                  # ditto
#   ./build_native.sh windows                # cross-compile windows/x86_64 (needs mingw-w64)
#   ./build_native.sh all                    # host + windows
#   ./build_native.sh linux windows          # explicit list
#
# Build type defaults to Release; override with BUILD_TYPE=Debug.
set -euo pipefail
cd "$(dirname "$0")/native"

BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then targets=(host); fi
if [[ "${targets[0]}" == "all" ]]; then targets=(host windows); fi

build_one() {
    local target="$1"
    local build_dir cmake_args=()
    case "$target" in
        host|linux|"")
            build_dir="cmake-build-host"
            ;;
        windows|win|mingw|windows-x86_64)
            build_dir="cmake-build-mingw-x86_64"
            cmake_args=(-DCMAKE_TOOLCHAIN_FILE="${PWD}/cmake/toolchain-mingw-x86_64.cmake")
            ;;
        *)
            echo "Unknown target: $target" >&2; exit 2;;
    esac
    echo "=== Building target: $target ==="
    cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${cmake_args[@]}"
    cmake --build "$build_dir" --config "$BUILD_TYPE" -j"$JOBS"
}

for t in "${targets[@]}"; do build_one "$t"; done

echo
echo "Built libraries:"
find build -type f \( -name '*.so' -o -name '*.dll' -o -name '*.dylib' \) -printf '  %p\n'
