# CMake toolchain for cross-compiling NeoBAE.dll for Windows x86_64 with mingw-w64.
set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_TRIPLET x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_TRIPLET}-gcc)
set(CMAKE_CXX_COMPILER ${_TRIPLET}-g++)
set(CMAKE_RC_COMPILER  ${_TRIPLET}-windres)
set(CMAKE_AR           ${_TRIPLET}-ar)
set(CMAKE_RANLIB       ${_TRIPLET}-ranlib)

set(CMAKE_FIND_ROOT_PATH /usr/${_TRIPLET})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static-link the mingw runtime so the produced .dll has no external
# libgcc_s/libwinpthread/libstdc++ dependencies.
set(CMAKE_C_FLAGS_INIT   "-static-libgcc")
set(CMAKE_CXX_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc")
