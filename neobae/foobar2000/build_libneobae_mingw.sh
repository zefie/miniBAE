#!/usr/bin/env bash
# Build libneobae.dll for the foobar2000 plugin (MinGW cross or native Windows).
# Output is copied into neobae/foobar2000/lib/ for the MSVC foo_neobae project.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-foobar2000}"
LIB_OUT="$(cd "$(dirname "$0")" && pwd)/lib"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DBAE_PLATFORM=foobar2000 \
  -DNEOBAE_STATIC=ON \
  -DNEOBAE_SHARED_LIBNEOBAE=ON \
  -DNEOBAE_BUILD_VCLIB=ON \
  -DBUILD_PLAYBAE=OFF \
  -DBUILD_ZEFIDI=OFF \
  -DBUILD_CLITOOLS=OFF \
  -DBUILD_NBEDITOR=OFF \
  -DBUILD_NBSTUDIO=OFF \
  -DBAE_DISABLE_MP3_ENCODER=ON \
  -DBAE_DISABLE_VORBIS_ENCODER=ON \
  -DBAE_DISABLE_OPUS_ENCODER=ON \
  -DBAE_DISABLE_FLAC_ENCODER=ON \
  "$@"

cmake --build "$BUILD_DIR" --target neobae -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "$LIB_OUT"
# Typical MinGW/CMake output locations
for f in \
  "$BUILD_DIR/bin/libneobae.dll" \
  "$BUILD_DIR/bin/libneobae.lib" \
  "$BUILD_DIR/libneobae.dll" \
  "$BUILD_DIR/libneobae.lib"
do
  if [[ -f "$f" ]]; then
    cp -f "$f" "$LIB_OUT/"
    echo "Copied $f -> $LIB_OUT/"
  fi
done

ls -la "$LIB_OUT"
