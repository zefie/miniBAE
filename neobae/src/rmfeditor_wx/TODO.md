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
- ~~Store bank (if not unmodified built-in bank) in NBS session~~
- ~~Revamp titlebar and statusbar handling~~
- ~~smarter dirty (modified) tracking~~
- ~~Sound Bank Editor~~
  - ~~need killswitch for bad ADSR~~
  - ~~need apply button~~
  - ~~make sure we load the edited bank (not original) into memory when switching to midi tab~~
  - ~~lots of bugs with sample editor~~
  - ~~needs context menus still~~
  - ~~compression issues in sample editor~~
  - ~~LFO broken~~
  - ~~Bank editor must force ZSB if using ZMF features~~
  - ~~Sample tree~~
- ~~Allow cut/copy/pasting notes~~


# 0.11a
- ~~Tell user why ZMF is required via tooltip over the status bar text~~
- ~~Add "Clone this Instrument" and "Alias this Instrument" when right clicking instrument in MIDI Editor~~
- ~~Add "Re-assign this Sample" and "Assign Sample to Additional Instrument" when right clicking samples in the MIDI editor~~
- ~~When loading a song, if the song has built in settings (ZMF), apply those settings to the editor UI (checking the menu option, and checking the Save settings to Song box)~~
  - ~~Reset state of settings menu and "Save Settings to Song" box upon "new" or loaded document~~

# 0.12a
- ~~Fix ZMF tooltip~~
- ~~Graphics and implementation for pitch envelope~~
- Do things more efficiently
  - ~~Optimize bank loading~~
  - Optimize bank rebuilding and instrument editor apply
  - Add dialog with progress bar when doing long duration (>2s) operations

# Future
- Allow cut/copy/pasting instruments
- Allow dragging samples to other instruments
  - Ask user if they want to move, or alias the sample
  - If its the only sample/alias of the source instrument and they choose move, prompt user:
  - Create a new empty sample for the instrument [instrument name]? Selecting No will destroy the old instrument.
  - Implement functions to create new empty sample in old instrument, reassign sample SNDID to target INST
  - and destroy old INST if requested
  - If instrument has more than one sample, we still have to modify both the source and dest INST to update SNDID
  - If the user CTRL-drags then we just alias the sample
  - alias is just adding that SNDID to the inst without removing it from the old
- More throughly test backwards compatiblity with BeatnikX, maybe even WebTV Plus
  - Put on back-burner for now
  - old 2021 miniBAE can play files (obv without mp3) that BeatnikX can't
  - try to modify our RMF generator so that we don't generate files that don't work on BeatnikX
    but still work on NeoBAE
  - ~~mod2rmf doesn't play as intended in BeatnikX, are we generating correctly? (edited RMF seems fine)~~
  - ~~Works fine in NeoBAE, but RMF files in particular NEED to be properly backwards compatible~~
  - ~~Change happened between 2025-08-23 and 2025-09-01 release (MIN_LOOP_SIZE dropped from 20 to 2)~~
- bankrecomp: wtv.hsb recomp prog 100 weirdness
- Interpolation configuration (none, linear, cubic, etc, current is just on/off)
- Neo Reverb for preview player
  - Custom .neoreverb support
  - means we need the reverb edit dialog from zefidi


# Harder stuff

- ~~Allow for automation like Volume to be able have a slide on it~~
  - ~~for example, to easily make a fadein or fadeout~~
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
