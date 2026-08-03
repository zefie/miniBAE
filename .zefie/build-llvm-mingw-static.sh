#!/bin/bash
# Get the version
COMMIT=$(git rev-parse --short HEAD 2>/dev/null)
DIRTY=$(git status --porcelain 2>/dev/null | head -n 1)
TAG_COMMIT=$(git rev-list --abbrev-commit --tags --max-count=1 2>/dev/null)
TAG=$(git describe --abbrev=0 --tags "$TAG_COMMIT" 2>/dev/null || true)
DATE=$(git log -1 --format=%cd --date=format:"%Y%m%d" 2>/dev/null)

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
/opt/llvm-mingw/wrapper.sh --cmake -DNEOBAE_STATIC=1 -DBAE_PLATFORM=SDL3 -DNEOBAE_BUILD_VCLIB=1 -DENABLE_MIDI_HW=1 "${@}"
QUEUE_EXIT=$?

# we now have build-aarch64-w64-mingw32, build-armv7-w64-mingw32, build-x86_64-w64-mingw32, and build-i686-w64-mingw32
# directories with the built libraries and executables. Now build the release package.
rm -rf out/
mkdir -p out/
if [ "${1}" == "--clean" ]; then
    echo " *** Cleaning build directories..."
    rm -rf build-*-w64-mingw32
fi

if [ "${QUEUE_EXIT}" != 0 ]; then
	exit "${QUEUE_EXIT}"
fi

for t in aarch64 armv7 x86_64 i686; do
    DEST="neobae-suite_windows_${t}_${VERSION}"
    cd "build-${t}-w64-mingw32/bin"
    echo " *** Packaging ${DEST}.zip..."
    zip -9 "../../out/${DEST}.zip" *.*
    cd "../.."
done

if [ "${1}" == "--clean" ]; then
    echo " *** Cleaning build directories..."
    rm -rf build-*-w64-mingw32
fi
