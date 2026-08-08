# How to build

These instructions were tested on Debian Linux (including WSL2). Package names may differ on other distributions.

## Before you start

- Run commands from the repository root unless the section says otherwise.
- CMake build artifacts are in `build/bin/` (or your chosen `-B` directory).
- Legacy Makefile build artifacts are in `neobae/bin/`.
- Keep `clean` separate from parallel Makefile builds. Do this:

```bash
make clean
make -j$(nproc)
```

Do not combine into one line like `make clean -j$(nproc)`.

By default (without `NOAUTO=1`), Makefile builds enable modern format support and SoundFont support via NeoBAE's FluidLite where applicable.

## What should I build?

- `playbae`: command-line player and test tool.
- `zefidi`: GUI player/editor utility.
- `nbeditor`: NeoBAE Editor (RMF/ZMF/HSB/ZSB editor; Dear ImGui + SDL3).
- Web build: WebAssembly output (`engine.js` + `engine.wasm`).

## Get the source

### Git (recommended)

```bash
git clone https://github.com/zefie/NeoBAE
cd NeoBAE
git submodule update --init --recursive
```

### Tarball/zip (manual dependency setup)

If you do not use git submodules, place third-party sources in these folders:

- [minimp3](https://github.com/lieff/minimp3) -> `neobae/src/thirdparty/minimp3`
- [FLAC](https://xiph.org/flac/) -> `neobae/src/thirdparty/flac`
- [OGG](https://xiph.org/ogg/) -> `neobae/src/thirdparty/libogg`
- [Vorbis](https://xiph.org/vorbis/) -> `neobae/src/thirdparty/libvorbis`
- [Opus](https://www.opus-codec.org/downloads/) -> `neobae/src/thirdparty/opus`
- [Opusfile](https://www.opus-codec.org/downloads/) -> `neobae/src/thirdparty/opusfile`
- [RtMidi](https://github.com/thestk/rtmidi) -> `neobae/src/thirdparty/rtmidi`
- [libg722](https://github.com/sippy/libg722) -> `neobae/src/thirdparty/libg722`
- [libxmp](https://github.com/libxmp/libxmp) -> `neobae/src/thirdparty/libxmp`
- [qoa](https://github.com/phoboslab/qoa) -> `neobae/src/thirdparty/qoa`
- [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) -> `neobae/src/thirdparty/imgui`

Notes:

- If you use `NOAUTO=1`, you only need third-party code for the features you enable.
- `imgui` is required to build `nbeditor`.

# Compiling NeoBAE

## cmake

The current recommended way to build the NeoBAE suite is with `cmake` from the repository root.

For available `-D` flags, see [CMakeFlags.md](CMakeFlags.md).

### Linux

```bash
sudo apt-get update
sudo apt-get install -y libsdl3-dev libsdl3-ttf-dev
cmake -B build .
cmake --build build --parallel $(nproc)
./build/bin/playbae -h
```

Optional packages for richer system-codec / tooling builds: `libsndfile-dev`, `libmp3lame-dev`, `libogg-dev`, `libvorbis-dev`, `libopus-dev`, `libopusfile-dev`, `libflac-dev`, `liblzma-dev`, `libxmp-dev`, `python3`.

#### Install (headers, library, tools)

```bash
cmake --install build --prefix /usr/local
# or: sudo cmake --install build --prefix /usr
```

This installs:

- library → `lib/libneobae.so*`
- tools → `bin/`
- public headers → `include/neobae/`
- CMake package → `lib/cmake/NeoBAE/` (`find_package(NeoBAE)`)

Components: `Runtime`, `Tools`, `Development` (select with `--component`).

#### Optional Debian packages

CPack can build `.deb` packages for **the distro you build on** (Depends come from that machine’s `dpkg-shlibdeps`). A Trixie build is for Trixie; rebuild on another release/distro for that target.

```bash
cd build
cpack -G DEB
# produces libneobae, neobae-tools, libneobae-dev
```

### Windows

I highly recommend building in my [docker toolchain](https://hub.docker.com/repository/docker/zefie/llvm-mingw)

To build just for x86_64 (from the repository root):

```bash
docker run --rm -it -v ./:/src zefie/llvm-mingw:latest .zefie/build-llvm-mingw-static_x86_64_only.sh
```

This uses CPack to write two zips under `out/`:

- `neobae-suite_windows_<arch>_<version>.zip` — flat tools + `libneobae.dll`
- `neobae-sdk_windows_<arch>_<version>.zip` — `include/neobae/`, `lib/` (import libs, `.def`, CMake package), `bin/libneobae.dll`

To build for all archs:

```bash
docker run --rm -it -v ./:/src zefie/llvm-mingw:latest .zefie/build-llvm-mingw-static.sh
```

This generates suite + sdk zips per architecture (8 archives) in `out/`.

## Legacy Makefiles

Some parts of NeoBAE can still be built, in separate parts, using the legacy Makefiles. Run these from `neobae/`.

### Linux: playbae (SDL3)

```bash
sudo apt-get update
sudo apt-get install -y libc6-dev libsdl3-dev libsndfile-dev libmp3lame-dev \
  libogg-dev libvorbis-dev libopus-dev libopusfile-dev libflac-dev liblzma-dev 
cd neobae
make clean
make USE_SDL3=1 -j$(nproc)
./bin/playbae -h
```

### Linux: zefidi GUI

```bash
sudo apt-get update
sudo apt-get install -y libsdl3-dev libsdl3-ttf-dev libsndfile-dev libmp3lame-dev \
  libogg-dev libvorbis-dev libopus-dev libopusfile-dev libflac-dev liblzma-dev
cd neobae
make clean
make -f Makefile.gui -j$(nproc)
./bin/zefidi
```

Output executable is in `neobae/bin/`.

### playbae (gcc)

```bash
cd neobae
make clean
make USE_SDL3=1 -j$(nproc)
```

### playbae (clang)

```bash
sudo apt-get install -y clang
cd neobae
make clean
make -f Makefile.clang -j$(nproc)
```

### zefidi GUI (base)

```bash
cd neobae
make clean
make -f Makefile.gui -j$(nproc)
```

### zefidi GUI with hardware MIDI (Linux)

Choose one of these:

- ALSA:

```bash
cd neobae
make clean
make -f Makefile.gui ENABLE_MIDI_HW=1 ENABLE_ALSA=1 -j$(nproc)
```

- JACK:

```bash
cd neobae
make clean
make -f Makefile.gui ENABLE_MIDI_HW=1 ENABLE_JACK=1 -j$(nproc)
```

- ALSA + JACK:

```bash
cd neobae
make clean
make -f Makefile.gui ENABLE_MIDI_HW=1 ENABLE_ALSA=1 ENABLE_JACK=1 -j$(nproc)
```

You must install the corresponding development packages for ALSA and/or JACK.

### Linux run notes for zefidi

- `zefidi` looks for `zenity`, then `kdialog`, then `yad` for file dialogs.
- Without one of those tools, `Open`, `Load Bank`, `Export`, and `Record` may not work.

### NeoBAE Editor

CMake (recommended; builds `nbeditor` when SDL3 and Dear ImGui sources are available):

```bash
sudo apt-get update
sudo apt-get install -y libsdl3-dev
cmake -B build -DBUILD_NBEDITOR=ON .
cmake --build build --parallel $(nproc) --target nbeditor
./build/bin/nbeditor
```

Legacy Makefile:

```bash
sudo apt-get update
sudo apt-get install -y libsdl3-dev libsndfile-dev \
  libogg-dev libvorbis-dev libopus-dev libopusfile-dev libflac-dev \
  liblzma-dev libmp3lame-dev libxmp-dev
cd neobae
make clean
make -f Makefile.nbeditor -j$(nproc)
./bin/nbeditor
```

## WebAssembly (Emscripten)

This target uses WebAudio and does not use SF2 support in `Makefile.emcc`.


```bash
sudo apt-get update
sudo apt-get install -y emscripten
cd neobae
make clean
make -f Makefile.emcc -j$(nproc)
cd bin
python -m http.server 8888
```

Then open `http://localhost:8888/`.

Notes:

- The WebAssembly build and old `musicObject` JS are separate systems.
- MPEG decode support is enabled in the WebAssembly build.
- SF2 support is supported with `Makefile.emcc-full`, but requires you to compile the following with emscripten manually: lame, ogg, opus, opusfile, vorbis, vorbisfile, and flac

## macOS

macOS build instructions are not yet documented in this guide.

## Modular build system (advanced)

The information below pertains to the old Makefile build system. For cmake flags, see [CMakeFlags.md](CMakeFlags.md)

To disable auto-enabled features and fully control options:

```bash
make NOAUTO=1 ...
```

### Common feature flags

- `MP3_DEC=1`: MP3 decode
- `MP3_ENC=1`: MP3 encode/export
- `FLAC_DEC=1`: FLAC decode
- `FLAC_ENC=1`: FLAC encode/export
- `VORBIS_DEC=1`: Vorbis decode
- `VORBIS_ENC=1`: Vorbis encode/export
- `OPUS_DEC=1`: Opus decode
- `OPUS_ENC=1`: Opus encode/export
- `OGG_SUPPORT=1`: OGG container support
- `KARAOKE=1`: MIDI karaoke support
- `ZMF_SUPPORT=1`: `.zmf` support
- `XMF_SUPPORT=1`: `.xmf` and `.mxmf` support
- `MTHC_SUPPORT=1`: Nokia compressed MIDI (MThc)
- `ADP_SUPPORT=1`: Nokia ADP G.722
- `ADX_SUPPORT=1`: CRI ADX
- `PLAYLIST=1`: GUI playlist support
- `J2ME_PATCH`: Patch for J2ME alternate drum channel
- `RETRO_RINGTONE_SUPPORT`: RTTTL, RNG, and RTX support
- `UNROLL_MIDI`: Unroll rolled MIDIs (eg `DialingWebTV.mid`)

### Synth/audio backend flags

- `SF2_SUPPORT=1`: enable SoundFont support
- `USE_FLUIDLITE=1`: use NeoBAE's FluidLite SoundFont backend
- `USE_SDL2=1` or `USE_SDL3=1`: select SDL backend (mutually exclusive)

### Debug/build behavior flags

- `DEBUG=1`: debug-friendly build options where supported
- `LDEBUG=1`: disable optimizations and keep debug symbols

### Other lesser-used flags
- `DISABLE_NOKIA`: Somewhat misleading, enabling this option disables the Nokia NRPN workarounds (eg disabling the ring track of a ringtone MIDI). The default (disabled) is recommended.
- `DISABLE_BEATNIK_SF2_NRPN`: Enabling this option disables processing of BAE NRPN events in SF2 mode. The default (disabled) is recommended.
- `USE_BUILTIN_PATCHES_WITH_SF2`: When enabled, loads the built-in HSB bank in the background along with the SF2 bank. The built-in bank is used with RMFs that call Bank 1. The default (enabled) is recommended.
- `FIX_SPAN_DC`: Fixes some issues with certain HSB banks and incorrect stereo panning. This option can be toggled at runtime. No adverse effects have been noted with this option, so the default (enabled) is recommended.
- `CLASSIC_CHORUS`: Enables the option to swap Chorus/Reverb processing order, bringing the player back to the 1.x style. This option can be toggled at runtime, and is disabled by default at runtime. The default (enabled) is recommended for compatiblity.
- `LZMA_SUPPORT`: Enables LZMA compression. Required for `ZMF_SUPPORT`.
- `CREATION_API`: Designed for use with applications that intend to create an RMF/ZMF file. It is recommended to use the Makefile defaults for this option.

### Important flag relationships

- `SF2_SUPPORT=1` forces `USE_FLUIDLITE=1`.
- `XMF_SUPPORT` requires Native DLS path (`USE_NATIVE_DLS=1`).
- Any Vorbis/FLAC/Opus enablement will force `OGG_SUPPORT=1`.
- `ZMF_SUPPORT=1` pulls in modern codec paths, as well as lzma.
- On MinGW CLI builds, use `USE_SDL3=1` to switch from DirectSound to SDL3.

## Troubleshooting

### Build command fails when cleaning with parallel jobs

Run clean and build as separate commands:

```bash
make clean
make -j$(nproc)
```

### zefidi crashes when opening MIDI device list on WSL

If `/dev/snd/seq` is missing, ALSA MIDI support can crash when opening MIDI device dropdowns. Build without `ENABLE_MIDI_HW=1` on those systems. With CMake, pass `-DBAE_DISABLE_MIDI_HARDWARE=ON`.

### I just want the original miniBAE without all this extra junk!

That's fine, and totally possible! Just build `playbae` with the following options:
- Linux: `make -f Makefile NOAUTO=1 MP3_DEC=1`
- Windows: `make -f Makefile.mingw NOAUTO=1 MP3_DEC=1`

This will produce a minimal playbae with only base miniBAE and MPEG support. No Nokia Patches, no FluidLite SoundFont backend, no modern codecs... Just plain old original miniBAE.
