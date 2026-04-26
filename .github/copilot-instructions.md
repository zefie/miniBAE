# NeoBAE Copilot Instructions

Purpose: keep changes small, correct, and project-aware.

## 1) Working Style (Karpathy Logic Adapted)

### Think First
- State assumptions before coding.
- If requirements are ambiguous, ask concise clarifying questions.
- Surface tradeoffs instead of silently choosing one path.
- Prefer the simplest viable fix over clever abstractions.

### Simplicity First
- Implement only what was requested.
- Avoid speculative features, generic frameworks, or premature abstractions.
- Keep code and diffs as small as possible while fully solving the task.

### Surgical Changes
- Touch only files and lines required for the request.
- Match existing local style and patterns.
- Do not refactor unrelated code unless explicitly asked.
- If you notice unrelated issues, mention them briefly instead of changing them.

### Goal-Driven Execution
- Translate requests into explicit success criteria.
- For multi-step work, define short verify points per step.
- Validate behavior after edits whenever feasible.
- If validation cannot be run, state exactly what was not verified.

## 2) NeoBAE Project Context

### Project Overview
- Project Name: NeoBAE (formerly miniBAE)
- Focus: Audio engine, RMF/ZMF format handling, and related tools.

### Architecture Snapshot
- Core audio engine lives under `neobae/src/BAE_Source/Common/`.
- Platform abstractions live under `neobae/src/BAE_Source/Platform/`.
- Main frontends include CLI/player tools and GUI paths under `neobae/src/gui/`.
- RMF editor code path is `neobae/src/rmfeditor_wx/`.

### Audio/Engine Facts
- Mixer uses high-precision intermediate buffers and hard saturation at final 16-bit output (no soft limiter).
- Many timing/rate values are 16.16 fixed-point; be careful with raw Hz vs fixed-point conversions.
- Manual memory APIs (`XNewPtr` / `XDisposePtr`) are common; check ownership and cleanup on new paths.
- Current voice cap is high (`MAX_VOICES`/`BAE_MAX_VOICES` mapped to 1024 on this branch).

### Format Handling Facts
- Detection is extension-first with content-signature fallback (`src/BAE_Source/Common/XFileTypes.c`).
- RMF/HSB and ZMF/ZSB share structure but differ by resource-map header and codec policy:
  - RMF/HSB header: `IREZ`
  - ZMF/ZSB header: `ZREZ`
- IREZ containers with modern codecs (FLAC/Vorbis/Opus) are invalid in runtime load paths; ZREZ is required.
- In RMF/HSB and ZMF/ZSB work, verify header, sample subtype, and fixed-point sample-rate handling.

### Known Quirks
- FluidSynth DLS path may log `Not a SoundFont file`; this is expected noise, not a fatal signal by itself.
- RMF playback/save bugs often involve file-type routing, stale resource reuse, split root-key flags, or sample-rate normalization.
- RMF editor is strong for playback/integration tasks but has limited full DAW-style authoring primitives.

## 3) Execution Rules for This Repository

- Do not run builds by default. The user handles builds manually (often external/docker).
- Only run build or compile commands if the user explicitly asks in that turn.
- Prefer focused static checks, code inspection, and targeted validations that do not require full project builds.
- When changing serialization/format code, preserve backward compatibility unless the request says otherwise.
- When creating new features, backward compatibility is the main priority, regardless of user prompt. If the feature cannot be implemented in a backward-compatible way, ask for clarification on how to proceed.
- Always use preprocessor conditionals to gate new and existing features, even if the user does not explicitly ask for it. This is to preserve the ability to turn off new features if they cause issues, and customizable builds.
- When updating `neobae/inc/Makefile.common`, also update `CMakeLists.txt` files to keep build systems in sync. This includes new source files, new preprocessor definitions, and new build targets.

## 4) Editing Checklist

Before edits:
- Confirm assumptions and scope.
- Identify exact files/symbols to touch.

During edits:
- Keep diffs minimal and request-focused.
- Preserve API behavior unless change is requested.

After edits:
- Re-scan impacted call paths.
- Validate edge cases relevant to the change.
- Report what was verified vs not verified.

## 5) Memory Usage

- Consult `/memories/repo/` notes for prior discoveries before deep investigations.
- Reuse known findings for RMF/ZMF, mixer behavior, codec handling, and editor quirks.
- Add concise new repository memory notes for non-obvious bugs, architectural constraints, or recurring gotchas discovered during work.

## 6) Creating NeoBAE Editor
- The editor is a separate executable with its own code path under `neobae/src/nbeditor/`.
- It shares core engine and format handling code but has its own UI and integration logic.
- When implementing editor features, ensure they do not regress existing RMF/HSB or ZMF/ZSB playback functionality. Use preprocessor conditionals to gate editor-specific code.
- The editor should be a master window that contains subwindows, docking, and a flexible UI layout.
- The editor will NOT contain a MIDI editor or piano roll at launch. Focus on core RMF/HSB and ZMF/ZSB editing features first, such as resource management, sample editing, and instrument manipulation. MIDI editing can be considered for future iterations.
- Use images in `.github/nbeditor_reference_images/` as visual references for the editor's intended UI design and layout. These images are not strict requirements but should guide the overall look and feel of the editor. Do not apply excessive "eye-candy" despite the reference images; prioritize a clean and functional UI that matches the spirit of the references without overcomplicating the design.

## 7) NBEditor Implementation Notes (2026-04)
- UI stack for the new editor path is Dear ImGui + SDL3 renderer backend. Do not add wxWidgets dependencies to nbeditor.
- Keep nbstudio and nbeditor build targets separate during migration. Do not replace nbstudio wiring until explicitly requested.
- nbeditor initial scope is MVP only:
  - Player window
  - Sample List window
  - Instrument List window
  - Context menu wiring is intentionally deferred
- Use compile-time gates for all nbeditor slices and windows:
  - `NBEDITOR`
  - `NBEDITOR_MVP`
  - `NBEDITOR_PLAYER_WINDOW`
  - `NBEDITOR_SAMPLE_LIST_WINDOW`
  - `NBEDITOR_INSTRUMENT_LIST_WINDOW`
- Build parity requirement:
  - Any `neobae/inc/Makefile.common` source/define changes for nbeditor must be mirrored in `CMakeLists.txt` nbeditor target wiring.
- Current MVP data flow expectations:
  - Transport/engine controls should use NeoBAE APIs directly (`BAEMixer_*`, `BAESong_*`).
  - Sample/Instrument list population should come from `BAERmfEditorDocument_*` APIs when a document is loaded.
- UX direction for player window:
  - Simple rectangular controls over ornamental skins.
  - Linear volume slider (not circular dial).
  - 16 channel mute checkboxes and voice meter are required in MVP.
  - Any oscilloscope should stay lightweight and optional-looking (small panel).