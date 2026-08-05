# NeoBAE

NeoBAE is a modernized continuation of miniBAE, the Beatnik Audio Engine. It combines the original real-time software synthesizer and compact audio engine with newer codecs, updated platform support, authoring tools, and multiple frontends for playback, conversion, and editing.

The project can be used as an embeddable audio engine, a command-line player and converter, a GUI player, a web target, and an RMF/ZMF authoring environment. For the original historical and technical background, see [miniBAE_README.md](miniBAE_README.md).

## Overview

- Real-time software synthesis with Beatnik-style banks, General MIDI playback, karaoke support, and live MIDI interaction.
- Multi-format playback and conversion across classic Beatnik formats, standard audio formats, retro ringtone formats, and modern codecs.
- Cross-platform targets for Linux, Windows, Android, WebAssembly, and macOS (via [Homebrew](https://brew.sh)).
- Multiple frontends ranging from the `playbae` CLI to the `zefidi` GUI and `nbeditor` editor.
- Modular build flags for trimming features or enabling optional integrations such as NeoBAE's FluidLite SoundFont backend or the Native DLS engine.
- [BAEScript](neobae/src/script/BAEScript_ReadMe.md) for manipulating songs without modification to the source file.
- Designed with a high emphasis on preserving retro compatibility while adding new features.

## Supported Formats

### Song and container formats

- MIDI: `.mid`, `.midi`
- Karaoke MIDI: `.kar` with lyrics processing
- RMI: `.rmi` including RMI files with embedded DLS when built with Native DLS, and SF2 support when built with NeoBAE's FluidLite
- RMF: `.rmf` for classic Beatnik Rich Music Format content
- ZMF: `.zmf` for RMF-style content with modern feature and codec support
- XMF and MXMF: `.xmf`, `.mxmf` mobileBAE formats, with full DLS support when built with Native DLS support
- MTHC: Nokia Compressed MIDI format (sometimes `.re`)
- MOD: import and conversion tooling via `mod2rmf`, powered by [libxmp](https://github.com/libxmp/libxmp)

### Retro ringtone formats

- iMelody: `.imy`, `.emy`
- Nokia binary ringtone: `.rng` (Basic Song / Temporary Song)
- RTTTL and RTX: `.rtttl`, `.rtx`

### Audio and sample formats

- PCM and uncompressed audio: `.wav`, `.aif`, `.aiff`, `.au`
- MPEG audio: `.mp2`, `.mp3`
- FLAC: `.flac`
- Ogg Vorbis: `.ogg`, `.oga`
- Opus: `.opus` and Ogg Opus content
- Quite OK Audio: `.qoa`
- WMA / MSAUDIO: `.wma` when built with WMA support
- ADP / ADPCM content: `.adp`
- CRI ADX: `.adx`

### Banks and instrument formats

- NeoBAE banks: `.hsb`, `.zsb`
- SoundFont: `.sf2`, `.sf3`, `.sfo` when built with NeoBAE's FluidLite
- DLS: `.dls` when built with Native DLS support

## Applications

- `playbae`: primary command-line player, renderer, and export tool for NeoBAE-supported content.
- `zefidi`: GUI player with playlists, visualization, channel controls, export features, and hardware MIDI integration where supported.
- `nbeditor`: RMF/ZMF/HSB/ZSB editor for instrument management, sample authoring, preview, and modern codec workflows. NeoBAE Editor is currently in early access and considered alpha quality.
- WebAssembly build: browser-targeted engine output for custom web players and interactive tools.
- Android app: mobile frontend under `neobae/src/NeoBAEDroid` for NeoBAE-based playback on Android.
- `libNeoBAE`: embeddable library output for integrating the engine into other applications.

### CLI tools

- `rmfinfo`: inspect RMF and ZMF structure, headers, and metadata.
- `rmf-instdump`: inspect instrument definition information from RMF and ZMF content.
- `mid2rmf`: Convert MIDI into RMF or ZMF.
- `rmf2mid`: Extract MIDI from RMF and ZMF.
- `mid2rmi`: wrap MIDI and Soundbank into RMI.
- `mod2rmf`: convert MOD tracker content into RMF or ZMF.
- `ringtone2mid`: convert retro ringtone formats into MIDI.
- `adp2wav`: decode Nokia ADP audio into WAV.
- `adx2wav`: decode CRI ADX audio into WAV.
- `songtool`: RMF and ZMF multitool: get song info, recompress samples, set loop points, apply gain, and more.
- `sf2-to-hsb`: convert SoundFont banks into NeoBAE HSB and ZSB banks.
- `bankrecomp`: Re-compress HSB/ZSB bank sample resources.
- `mthc_decomp`: convert Nokia Compressed MIDI into standard MIDI.

## Quick Start

CMake is the recommended build path. Run from the repository root after initializing submodules.

### Linux (cmake)

```bash
cmake -B build .
cmake --build build --parallel $(nproc)
./build/bin/playbae -h
```

### WebAssembly build (legacy Makefile)

```bash
cd neobae
make clean
make -f Makefile.emcc -j$(nproc)
```

For platform-specific prerequisites, Windows cross-compilation, legacy Makefiles, debug builds, and optional feature flags, see [HowToBuild.md](HowToBuild.md) and [CMakeFlags.md](CMakeFlags.md).

## Project Layout

- `neobae/`: build system, frontends, CLI tools, banks, main sources, and examples.
  - `src/BAE_Source/Common/`: core synthesis, mixers, loaders, and shared engine code.
  - `src/BAE_Source/Platform/`: platform abstractions and audio backend glue.
  - `src/NeoBAEDroid/`: Android application source.
- `content/`: sample media for playback and format testing.

## Additional Reading

- [miniBAE_README.md](miniBAE_README.md): original background, historical context, and legacy overview.
- [HowToBuild.md](HowToBuild.md): detailed build instructions and feature configuration notes.
- [BAEScript_ReadMe.md](neobae/src/script/BAEScript_ReadMe.md): contains BAEScript documentation.
- [ACKNOWLEDGEMENTS](neobae/ACKNOWLEDGEMENTS): contributors and third-party credits.

## License

NeoBAE (zefie's modifications) is licensed under LGPL-3.0.

Original Beatnik miniBAE code remains under BSD-3-Clause.

See [LICENSE](LICENSE), [LICENSE.BSD](LICENSE.BSD), and [NOTICE](NOTICE) for details.

Native DLS support uses code ported from [RetroDLS](https://github.com/Magstic/RetroDLS), used under MIT license, copyright (c) 2026 Magstic
