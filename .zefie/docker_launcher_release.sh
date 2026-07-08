#!/bin/bash
THEROOT="$(realpath "$(dirname "${0}")/..")"
docker run --rm -it -v ${THEROOT}:/src zefie/llvm-mingw:latest /src/.zefie/build-llvm-mingw-static.sh -DBUILD_NBEDITOR=OFF
