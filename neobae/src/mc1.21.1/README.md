# NeoBAE for Minecraft - NeoForge 1.21.1

A drop-in **decoder upgrade** for the [Etched](https://modrinth.com/mod/etched)
mod. When this mod is installed alongside Etched, it transparently replaces
Etched's built-in OGG/WAV/MP3 cascade with the NeoBAE (Beatnik Audio Engine)
native decoder, adding support for:

| Format          | Etched alone | + NeoBAE |
| --------------- | :----------: | :------: |
| MP3             | yes          | yes      |
| OGG Vorbis      | yes          | yes      |
| WAV             | yes          | yes      |
| **FLAC**        | no           | **yes**  |
| **MIDI**        | no           | **yes**  |
| **RMF / RMI**   | no           | **yes**  |
| **XMF**         | no           | **yes**  |
| **OGG Opus**    | no           | **yes**  |
| **AIFF / AU**   | no           | **yes**  |
| **MTHC** (Nokia compressed MIDI) | no | **yes** |

You still use Etched's etching table, Etched's discs, Etched's UI - there are
no new items, blocks, or recipes from this mod. Drop it in and the new
formats just start working.

## License

This subproject is **GPL-3.0-only** because [Etched is GPL-3.0-only](https://github.com/jacksonhardaway/etched).
The NeoBAE / miniBAE engine itself stays BSD-3-Clause; only the Minecraft
integration glue (everything in this folder) is GPL. See [LICENSE](LICENSE)
and [NOTICE](NOTICE).

## How it works

A [mixin](https://docs.neoforged.net/docs/advanced/mixins) (a runtime patch
into another mod's compiled code) injects at the start of Etched's
`AbstractOnlineSoundInstance.getStream(...)`. If the NeoBAE native library
loaded successfully, our injection takes over the entire decoder path:

1. Etched downloads the URL bytes as usual (we reuse `SoundCache` and `AudioSource`).
2. We hand the byte stream to NeoBAE, which auto-detects the format.
3. NeoBAE produces 44.1 kHz stereo S16LE PCM on demand into Minecraft's
   `SoundEngine` via a `NeoBAEAudioStream`.

If the native library fails to load, the mixin does nothing and Etched's
original decoder runs - the worst case is "this mod silently does nothing".

## Building

You need:

- **JDK 21** (Temurin recommended)
- **CMake >= 3.20**, a C99 compiler, **Python 3** (for the native build)
- An **Etched 5.0.x jar** for compile-time linkage (see step 2)

### 1. Native library (per OS/arch you ship for)

```sh
cd neobae/src/mc1.21.1
./build_native.sh                   # outputs native/build/<os>/<arch>/libNeoBAE.{so|dll|dylib}
```

Cross-compile for Windows from Linux:

```sh
CC=x86_64-w64-mingw32-gcc \
  cmake -S native -B native/cmake-build-win64 \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
cmake --build native/cmake-build-win64 -j
```

### 2. Etched jar for compilation

Pick one and uncomment the corresponding pair of lines in `build.gradle`'s
`dependencies` block:

- **Local jar (most reliable):** download `etched-5.0.1.jar` from Modrinth
  into `./libs/`, then enable the `files('libs/etched-5.0.1.jar')` lines.
- **JitPack:** enable the `com.github.jacksonhardaway:etched:5.0.1` lines
  (Gradle will fetch and build from the GitHub repo on first build).
- **Maven Local:** run `./gradlew publishToMavenLocal` inside an Etched
  checkout, then enable the `gg.moonflower:etched:5.0.1` lines.

These are `compileOnly + runtimeOnly`, not `implementation` - we never ship
Etched's classes in our jar.

### 3. Build the mod

```sh
./gradlew build                     # produces build/libs/neobaemc-0.1.0.jar
```

Install Etched + this jar into `mods/` on both client and server.

## Limitations

- **Single-disc playback only.** NeoBAE's mixer is a process-wide singleton,
  so two NeoBAE-decoded discs playing at the same time will mix into the same
  render call and the two streams will fight over samples. The common case
  (one jukebox playing within earshot) works fine. A per-stream-mixer refactor
  is the right fix; not done yet.
- **No NeoBAE-side caching.** Bytes come through Etched's `SoundCache`; we
  re-decode on each play. Fine for typical disc-length tracks; you'd notice
  on multi-hour streams.
- **All-or-nothing override.** When NeoBAE is loaded it owns the decode path
  for all online sounds. If NeoBAE somehow fails to recognise the bytes the
  stream resolves to silence rather than falling back to Etched's chain.
  NeoBAE supports a strict superset of Etched's formats so this should
  effectively never trigger.
- **Native library required.** Without `libNeoBAE.{so|dll|dylib}` on the
  expected resource path, the mixin no-ops and Etched runs unchanged.

## File map

| Path | Purpose |
| ---- | ------- |
| [native/](native/) | C source + CMake for `libNeoBAE` |
| [native/neobaemc_jni.c](native/neobaemc_jni.c) | JNI shim, adapted from NeoBAEDroid |
| [build_native.sh](build_native.sh) | One-shot native build |
| [src/main/java/com/zefie/neobaemc/audio/](src/main/java/com/zefie/neobaemc/audio/) | `Mixer`, `Song`, `Sound`, `LoadResult`, `NativeLoader`, `NeoBAEAudioStream` |
| [src/main/java/com/zefie/neobaemc/mixin/AbstractOnlineSoundInstanceMixin.java](src/main/java/com/zefie/neobaemc/mixin/AbstractOnlineSoundInstanceMixin.java) | The actual override hook into Etched |
| [src/main/java/com/zefie/neobaemc/etched/EtchedIntegration.java](src/main/java/com/zefie/neobaemc/etched/EtchedIntegration.java) | Init logging only |
| [src/main/resources/neobaemc.mixins.json](src/main/resources/neobaemc.mixins.json) | Mixin configuration |
| [src/main/resources/META-INF/neoforge.mods.toml](src/main/resources/META-INF/neoforge.mods.toml) | Mod metadata + hard-dep on Etched |
