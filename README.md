# NeoBAE

NeoBAE is a modernized continuation of miniBAE, the Beatnik Audio Engine. It combines the original real-time software synthesizer and compact audio engine with newer codecs, updated platform support, authoring tools, and multiple frontends for playback, conversion, and editing.

The project can be used as an embeddable audio engine, a command-line player and converter, a GUI player, a web target, and an RMF/ZMF authoring environment. For the original historical and technical background, see [miniBAE_README.md](miniBAE_README.md).

## Overview

- Real-time software synthesis with Beatnik-style banks, General MIDI playback, karaoke support, and live MIDI interaction.
- Multi-format playback and conversion across classic Beatnik formats, standard audio formats, retro ringtone formats, and modern codecs.
- Cross-platform targets for Linux, Windows, Android, WebAssembly, and macOS (via [Homebrew](https://brew.sh)).
- Multiple frontends ranging from the `playbae` CLI to the `zefidi` GUI and `nbstudio` editor.
- Modular build flags for trimming features or enabling optional integrations such as FluidSynth-backed SoundFont and DLS support.
- [BAEScript](neobae/src/script/BAEScript_ReadMe.md) for manipulating songs without modification to the source file.
- Designed with a high emphasis on preserving retro compatibility while adding new features.

## Supported Formats

### Song and container formats

- MIDI: `.mid`, `.midi`
- Karaoke MIDI: `.kar` with lyrics processing
- RMI: `.rmi` including RMI files with embedded DLS and SF2 when built with FluidSynth support
- RMF: `.rmf` for classic Beatnik Rich Music Format content
- ZMF: `.zmf` for RMF-style content with modern feature support
- XMF and MXMF: `.xmf`, `.mxmf` mobileBAE formats, with full DLS support when built with FluidSynth support
- MTHC: Nokia Compressed MIDI format
- MOD: import and conversion tooling via `mod2rmf`

### Retro ringtone formats

- iMelody: `.imy`, `.emy`
- Nokia binary ringtone: `.rng`
- RTTTL and RTX: `.rtttl`, `.rtx`

### Audio and sample formats

- PCM and uncompressed audio: `.wav`, `.aif`, `.aiff`, `.au`
- MPEG audio: `.mp2`, `.mp3`
- FLAC: `.flac`
- Ogg Vorbis: `.ogg`, `.oga`
- Opus: `.opus` and Ogg Opus content
- Quite OK Audio: `.qoa`
- ADP / ADPCM content: `.adp`

### Banks and instrument formats

- NeoBAE banks: `.hsb`, `.zsb`
- SoundFont: `.sf2`, `.sf3`, `.sfo` when built with FluidSynth support
- DLS: `.dls` when built with FluidSynth support

## Applications

- `playbae`: primary command-line player, renderer, and export tool for NeoBAE-supported content.
- `zefidi`: GUI player with playlists, visualization, channel controls, export features, and hardware MIDI integration where supported.
- `nbstudio`: RMF/ZMF/HSB/ZSB editor for instrument management, sample authoring, preview, and modern codec workflows. NBStudio is currently in early access and considered alpha quality.
- WebAssembly build: browser-targeted engine output for custom web players and interactive tools.
- Android app: mobile frontend under `neobae/src/NeoBAEDroid` for NeoBAE-based playback on Android.
- `libNeoBAE`: embeddable library output for integrating the engine into other applications.

### CLI tools

- `rmfinfo`: inspect RMF and ZMF structure, headers, and metadata.
- `rmf-instdump`: inspect instrument definition information from RMF and ZMF content.
- `mid2rmf`: convert MIDI into RMF and ZMF.
- `rmf2mid`: convert RMF back to MIDI.
- `mid2rmi`: wrap MIDI and Soundbank into RMI.
- `mod2rmf`: convert MOD tracker content into RMF and ZMF workflows.
- `ringtone2mid`: convert retro ringtone formats into MIDI.
- `adp2wav`: decode ADP audio into WAV.
- `songtool`: RMF and ZMF multitool: get song info, recompress samples, set loop points, apply gain, and more.
- `sf2-to-hsb`: convert SoundFont banks into NeoBAE HSB and ZSB banks.
- `bankrecomp`: recompress bank sample resources.
- `mthc_decomp`: convert Nokia Compressed MIDI into standard MIDI.

## Quick Start

All build artifacts are generated from the `neobae/` tree.

### Linux

```bash
cd neobae
make clean
make USE_SDL3=1 -j$(nproc)
./bin/playbae -h
```

### GUI build

```bash
cd neobae
make clean
make -f Makefile.gui -j$(nproc)
```

### WebAssembly build

```bash
cd neobae
make clean
make -f Makefile.emcc -j$(nproc)
```

For platform-specific prerequisites, Windows cross-compilation, debug builds, and optional feature flags, see [HowToBuild.md](HowToBuild.md).

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

NeoBAE (zefie's modifications) is licensed under GPL-3.0.

Original Beatnik miniBAE code remains under BSD-3-Clause.

See [LICENSE](LICENSE) and [NOTICE](NOTICE) for details.
