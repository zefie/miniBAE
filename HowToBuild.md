# How to build

These instructions were tested on Debian Linux (including WSL2). Package names may differ on other distributions.

## Before you start

- Run commands from the repository root unless the section says otherwise.
- Build artifacts are in `neobae/bin/`.
- Keep `clean` separate from parallel builds. Do this:

```bash
make clean
make -j$(nproc)
```

Do not combine into one line like `make clean -j$(nproc)`.

By default (without `NOAUTO=1`), builds enable modern format support and SoundFont via FluidSynth where applicable.

## What should I build?

- `playbae`: command-line player and test tool.
- `zefidi`: GUI player/editor utility.
- `nbstudio`: NeoBAE Studio (RMF editor).
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
- Linux only: modified [FluidSynth](https://github.com/zefie/fluidsynth) -> `neobae/src/thirdparty/fluidsynth`

Notes:

- If you use `NOAUTO=1`, you only need third-party code for the features you enable.

# Compiling NeoBAE

## cmake

The current recommended way to build the NeoBAE suite is with `cmake`.

### Linux
```bash
sudo apt-get update
sudo apt-get install -y wx3.2-headers libsdl3-dev
cd neobae
mkdir build
cmake -B build .
cmake --build build --parallel $(nproc)
./build/bin/playbae -h
```
### Windows

I highly recommend building in my [docker toolchain](https://hub.docker.com/repository/docker/zefie/llvm-mingw)

To build just for x86_64:
```bash
cd neobae
docker run --rm -it -v ./:/src zefie/llvm-mingw:latest .zefie/build-llvm-mingw-static_x86_64_only.sh
```
This will generate a zip in the `neobae/out` folder.

To build for all archs:
```bash
cd neobae
docker run --rm -it -v ./:/src zefie/llvm-mingw:latest .zefie/build-llvm-mingw-static.sh
```

This will generate 4 zips in the `neobae/out` folder, one pertaining to each architechure.


## Legacy Makefiles

NeoBAE can still be built in seperate parts using the legacy Makefiles.

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

### Windows cross-compile from Linux/WSL: playbae (DirectSound)

```bash
sudo apt-get update
sudo apt-get install -y binutils-mingw-w64-x86_64 g++-mingw-w64-x86_64 \
  g++-mingw-w64-x86_64-posix g++-mingw-w64-x86_64-win32 gcc-mingw-w64-base \
  gcc-mingw-w64-x86_64 gcc-mingw-w64-x86_64-posix \
  gcc-mingw-w64-x86_64-posix-runtime gcc-mingw-w64-x86_64-win32 \
  gcc-mingw-w64-x86_64-win32-runtime mingw-w64-common mingw-w64-x86_64-dev \
  libz-mingw-w64-dev
cd neobae
make clean
make -f mingw/Makefile -j$(nproc)
```

Output executable is in `neobae/bin/`.

## Linux builds

### Optional but recommended: build FluidSynth from source

Default feature sets expect FluidSynth support (SF2/SF3/SFO and limited DLS). On Linux, using zefie's modified FluidSynth tree gives best compatibility.

```bash
cd neobae/src/thirdparty/fluidsynth
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

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

## Windows builds (cross-compile on Linux/WSL)

### playbae via MinGW

- DirectSound build:

```bash
cd neobae
make clean
make -f mingw/Makefile -j$(nproc)
```

- SDL3 build:

```bash
cd neobae
make clean
make -f mingw/Makefile USE_SDL3=1 -j$(nproc)
```

### zefidi GUI via MinGW

```bash
cd neobae
make clean
make -f mingw/Makefile.gui -j$(nproc)
```

Notes:

- MinGW GUI dependencies for SDL are bundled in this repository.
- Hardware MIDI support is enabled for MinGW GUI builds.
- Copy outputs from `neobae/bin/` to Windows to run.

## NeoBAE Studio (RMF editor)

### Linux

```bash
sudo apt-get update
sudo apt-get install -y libsdl3-dev libsdl3-ttf-dev libsndfile-dev \
  libogg-dev libvorbis-dev libopus-dev libopusfile-dev libflac-dev \
  liblzma-dev libmp3lame-dev libxmp-dev
cd neobae
make clean
make -f Makefile.nbstudio -j$(nproc)
```

### Windows (Visual Studio)

- Open `neobae/src/nbstudio/vs2022/nbstudio.vsxproj`.
- In Developer PowerShell, within the `neobae/src/nbstudio/vs2022` folder, run `vcpkg install --triplet x64-windows`.
- Build in Visual Studio.

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
- SF2 support is supported with `Makefile.emcc-full`, but requires you to compile the following with emscripten manually: lame, ogg, opus, opusfile, vorbis, vorbisfile, flac, libsndfile, and fluidsynth

## macOS

macOS build instructions are not yet documented in this guide.

## Modular build system (advanced)

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
- `PLAYLIST=1`: GUI playlist support
- `J2ME_PATCH`: Patch for J2ME alternate drum channel
- `RETRO_RINGTONE_SUPPORT`: RTTTL, RNG, and RTX support
- `UNROLL_MIDI`: Unroll rolled MIDIs (eg `DialingWebTV.mid`)

### Synth/audio backend flags

- `SF2_SUPPORT=1`: enable SoundFont support
- `USE_FLUIDSYNTH=1`: use FluidSynth backend
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

- `SF2_SUPPORT=1` forces `USE_FLUIDSYNTH=1`.
- `XMF_SUPPORT` requires FluidSynth path (`USE_FLUIDSYNTH=1`).
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

If `/dev/snd/seq` is missing, ALSA MIDI support can crash when opening MIDI device dropdowns. Build without `ENABLE_MIDI_HW=1` on those systems.

### FluidSynth logs `Not a SoundFont file` while loading DLS

That message is expected in this project's DLS path. Treat later errors as the real failure signal.

### I just want the original miniBAE without all this extra junk!

That's fine, and totally possible! Just build `playbae` with the following options:
- Linux: `make -f Makefile NOAUTO=1 MP3_DEC=1`
- Windows: `make -f Makefile.mingw NOAUTO=1 MP3_DEC=1`

This will produce a minimal playbae with only base miniBAE and MPEG support. No Nokia Patches, no Fluidsynth, no modern codecs... Just plain old original miniBAE.
