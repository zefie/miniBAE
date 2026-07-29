#!/bin/bash
THEROOT="$(realpath "$(dirname "${0}")/..")"
docker run --rm -it -v ${THEROOT}:/src zefie/llvm-mingw:latest .zefie/build-llvm-mingw-static_x86_64_only.sh -DBUILD_CLITOOLS=0 -DBUILD_NBSTUDIO=0 -DBUILD_NBEDITOR=0 "${@}"
