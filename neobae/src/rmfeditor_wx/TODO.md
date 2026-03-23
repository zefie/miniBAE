# 0.08a
- ~~Resizable tracks (allow user to expand or shrink the track by click/dragging the far right end of the ruler)~~
- ~~Make piano in bank editor and instrument editor scrollable for full range~~
- ~~Start from nothing (currently the editor requires you to load a file)~~
- ~~Implement ZSB (Zefie Sound Bank), same deal as RMF/ZMF, used for newer codecs. Uses ZREZ.~~
- ~~Cannot play 1:0 or 2:0 in MIDI Editor (also affects exports, but works on re-import)~~
- ~~Default piano roll scroll to C5 center when there is no midi data~~
- ~~Hide bank editor behind warning that it is incomplete and broken "This feature is incomplete and many functions are not yet implemented or not yet functioning correctly. By continuing into the Bank Editor you agree that you understand this." with "I Understand" button~~
- ~~Replacing a sample does not update the original sample node in the tree.~~
  - ~~Creates a new duplicate sample entry, and only the currently edited instrument is updated to use it, leaving other instruments still pointing to the old sample.~~
- ~~Center instrument editor dialog piano to C4~~
- ~~Update Center to C5 to Center to C4~~
- ~~Fix pitch issues with bank compressor (MP3 low pitch, Opus high pitch, Opus RT only some detuned)~~

# 0.09a
- ~~LZMA support for ZMF/NBS/C_SND~~
- ~~LFO issue (instrument dialog)~~
- ~~Settings menu not working on windows? Why?~~
- ~~Dark mode!~~

# 0.10a
- ~~Codec bitrate ignored in instrument editor sample dialog?~~
- ~~ADPCM wrong pitch (detuned low)~~
- ~~Fix loop markers covered the note field~~
- ~~Revamp CC tracks in piano roll~~

# Future
- More throughly test backwards compatiblity with BeatnikX, maybe even WebTV Plus
  - Put on back-burner for now
  - old 2021 miniBAE can play files (obv without mp3) that BeatnikX can't
  - would have to undo changes actually done by Beatnik to 'retrograde' the engine to the old buggy state
    if we want to really be 1:1
  - ~~mod2rmf doesn't play as intended in BeatnikX, are we generating correctly? (edited RMF seems fine)~~
  - ~~Works fine in NeoBAE, but RMF files in particular NEED to be properly backwards compatible~~
  - ~~Change happened between 2025-08-23 and 2025-09-01 release (MIN_LOOP_SIZE dropped from 20 to 2)~~
- bankrecomp: wtv.hsb recomp prog 100 weirdness
- Graphics for pitch envelope
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)
- Neo Reverb for preview player
  - Custom .neoreverb support
  - means we need the reverb edit dialog from zefidi


# Harder stuff
- Sound Bank Editor
  - ~~need killswitch for bad ADSR~~
  - ~~need apply button~~
  - make sure we load the edited bank (not original) into memory when switching to midi tab
  - lots of bugs with sample editor
  - needs context menus still
  - musicial keyboard vs form fields
  - ~~LFO broken~~

- Allow for automation like Volume to be able have a slide on it
  - for example, to easily make a fadein or fadeout
  - the edit dialog could have "start (item)" "end (item)"

- Externally imported MP3 samples (not encoded by us) may have a gap
  - How to address without breaking backwards compatiblity with the decoder?

- Allow configuration of SysEx and 'non-standard' CC commands
  - Where do we even put this stuff?  

- MIDI In (record to track)
  - Use RtMidi
  - Hook alsa/jack/winmm as we do with Zefidi
  - Allow MIDI in for instrument dialog preview
  - Need to find my MIDI keyboard

# Maybe (really hard stuff)
- SF2/DLS support
  - How? FluidSynth handles all of that.
  - Do we try to convert instrument data? But the ADSR is different in RMF/ZMF/HSB.
