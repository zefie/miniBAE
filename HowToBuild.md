## How to compile
All compilation was tested on **Debian Linux 11 (Buster) amd64**
Package names and commands may vary if using a different OS.

#### Setup & Compile Linux 32-bit ANSI (no sound card support)
- Install i386 support (one time):
    - `dpkg --add-architecture i386`
    - `apt-get update`
    - `apt-get install libc6-dev-x32`
- Build playbae
    - `cd minibae/Tools/playbae`
    - `make clean all`
- Using Build:
    - Run `./playbae -h` for information on usage

#### Setup & Compile Linux 32-bit ANSI with clang (no sound card support)
- Install i386 support (one time):
    - `dpkg --add-architecture i386`
    - `apt-get update`
    - `apt-get install libc6-dev-x32 clang`
- Build playbae
    - `cd minibae/Tools/playbae`
    - `make -f Makefile.clang clean all`
- Using Build:
    - Run `./playbae -h` for information on usage

#### Setup & Compile Win32 mingw build (with DirectSound support)
- Install mingw32 (one time):
    - `apt-get update`
    - `apt-get install binutils-mingw-w64-i686 g++-mingw-w64-i686 g++-mingw-w64-i686-posix g++-mingw-w64-i686-win32 gcc-mingw-w64-base gcc-mingw-w64-i686 gcc-mingw-w64-i686-posix gcc-mingw-w64-i686-posix-runtime gcc-mingw-w64-i686-win32 gcc-mingw-w64-i686-win32-runtime mingw-w64-common mingw-w64-i686-dev`
- Build playbae
    - `cd minibae/Tools/playbae`
    - `make -f Makefile.mingw clean all`
- Using Build:
    - Copy `playbae.exe` to a Windows system
    - Run `playbae.exe -h` for information on usage

#### Setup & Compile Emscripten WASM32 build (no sound card support)
- Install emscripten (one time):
    - `apt-get update`
    - `apt-get install emscripten`
- Build playbae
    - `cd minibae/Tools/playbae`
    - `make -f Makefile.emcc clean all`
- Using Build:
    - Copy `minibae-audio.js`, `minibae-audio.htm`, and `playbae.js` to a web server (cannot use local filesystem due to browser security)
    - Modify `minibae-audio.htm` to point to desired audio file (and optionally patches.hsb)
    - Load in browser, wait for conversion, press play on player when the button is no longer greyed out
    - Emscripten build is WIP, usage of browser debugger/inspector suggested
    - **NOTE:** The Emscripten build and the old musicObject JS are completely different, and not compatible with each other.
