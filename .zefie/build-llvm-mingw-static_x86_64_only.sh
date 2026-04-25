#!/bin/bash
# Get the version
COMMIT=$(git rev-parse --short HEAD 2>/dev/null)
DIRTY=$(git status --porcelain 2>/dev/null | head -n 1)
TAG_COMMIT=$(git rev-list --abbrev-commit --tags --max-count=1 2>/dev/null)
TAG=$(git describe --abbrev=0 --tags "$TAG_COMMIT" 2>/dev/null || true)
DATE=$(git log -1 --format=%cd --date=format:"%Y%m%d" 2>/dev/null)

checkexit() {
	if [ ${1} != 0 ]; then
		exit ${1}
	fi
}

if [[ -z "$COMMIT" ]]; then
  # No git metadata (export tarball). Fallback to date.
  if [[ -z "$DATE" ]]; then
    DATE=$(date +%Y%m%d)
  fi
  VERSION="$DATE"
else
  if [[ -n "$TAG" ]]; then
    # If HEAD equals tag commit, use the tag (strip leading v)
    if [[ "$COMMIT" == "$TAG_COMMIT" ]]; then
      VERSION="${TAG#v}"
    else
      VERSION="git-$COMMIT"
    fi
  else
    VERSION="git-$COMMIT"
  fi

  if [[ -n "$DIRTY" ]]; then
    VERSION="${VERSION}-dirty"
  fi
fi

# Build the project
export TARGET=x86_64-w64-mingw32
export TOOLCHAIN_PREFIX="/opt/llvm-mingw"
export PATH="${TOOLCHAIN_PREFIX}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export CC="${TARGET}-clang"
export CXX="${TARGET}-clang++"
export RC="${TARGET}-windres"
export AR="${TARGET}-ar"
export RANLIB="${TARGET}-ranlib"
export STRIP="${TARGET}-strip"
export OBJCOPY="${TARGET}-objcopy"
export OBJDUMP="${TARGET}-objdump"
export PKG_CONFIG="/usr/bin/pkgconf"
export PKG_CONFIG_LIBDIR="${TOOLCHAIN_PREFIX}/${TARGET}/lib/pkgconfig:${TOOLCHAIN_PREFIX}/${TARGET}/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${TOOLCHAIN_PREFIX}/${TARGET}"
export CFLAGS="-march=x86-64 -mtune=znver4"
export CXXFLAGS="$CFLAGS"

mkdir -p build-${TARGET}
pushd build-${TARGET}

cmake .. \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_RC_COMPILER="${RC}" \
    -DCMAKE_INSTALL_PREFIX="${TOOLCHAIN_PREFIX}/${TARGET}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOBAE_STATIC=1 -DBAE_PLATFORM=WinOS -DNEOBAE_BUILD_VCLIB=1 -DENABLE_MIDI_HW=1
checkexit $?

cmake --build . --config Release -- -j$(nproc)
checkexit $?

popd

# we now have build-aarch64-w64-mingw32, build-armv7-w64-mingw32, build-x86_64-w64-mingw32, and build-i686-w64-mingw32
# directories with the built libraries and executables. Now build the release package.
rm -rf out/
mkdir -p out/
if [ "${1}" == "--clean" ]; then
    echo " *** Cleaning build directories..."
    rm -rf build-*-w64-mingw32
fi

checkexit "${QUEUE_EXIT}";

DEST="neobae-suite_windows_x86_64_${VERSION}"
cd "build-x86_64-w64-mingw32/bin"
echo " *** Packaging ${DEST}.zip..."
zip -9 "../../out/${DEST}.zip" *.*
cd "../.."

if [ "${1}" == "--clean" ]; then
    echo " *** Cleaning build directories..."
    rm -rf build-*-w64-mingw32
fi
