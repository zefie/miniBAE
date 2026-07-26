package com.zefie.neobaemc.audio;

/**
 * Wrapper around a NeoBAE song (MIDI/RMF/XMF/RMI/etc.).
 * Adapted from NeoBAEDroid's Song.java (script + meta-event hooks removed -
 * they are not used by the Minecraft music-disc flow).
 */
public class Song {
    private long mReference;
    private Mixer mMixer;

    private native long _newNativeSong(long mixerReference);
    private native int  _loadSong(long songReference, String path);
    private native int  _loadSongFromMemory(long songReference, byte[] data);
    private native int  _loadRmiFromMemory(long songReference, byte[] data, boolean useEmbeddedBank);
    private native int  _prerollSong(long songReference);
    private native int  _startSong(long songReference);
    private native void _stopSong(long songReference, boolean deleteSong);
    private native int  _pauseSong(long songReference);
    private native int  _resumeSong(long songReference);
    private native boolean _isSongPaused(long songReference);
    private native boolean _isSongDone(long songReference);
    private static native int _setSongVolume(long songReference, int fixedVolume);
    private static native int _getSongVolume(long songReference);
    private static native int _getSongPositionUS(long songReference);
    private static native int _setSongPositionUS(long songReference, int us);
    private static native int _getSongLengthUS(long songReference);
    private static native int _setSongLoops(long songReference, int numLoops);

    Song(Mixer mixer) {
        mMixer = mixer;
        mReference = _newNativeSong(mMixer.mReference);
        _setSongVolume(mReference, 65536); // 1.0 in 16.16
    }

    Song(Mixer mixer, long nativeReference) {
        mMixer = mixer;
        mReference = nativeReference;
        _setSongVolume(mReference, 65536);
    }

    public int load(String path) { return _loadSong(mReference, path); }
    public int loadFromMemory(byte[] data) { return _loadSongFromMemory(mReference, data); }
    public int loadRmiFromMemory(byte[] data, boolean useEmbeddedBank) { return _loadRmiFromMemory(mReference, data, useEmbeddedBank); }
    public int preroll() { return _prerollSong(mReference); }
    public int start()   { return _startSong(mReference); }
    public void stop(boolean deleteSong) { _stopSong(mReference, deleteSong); }
    public int pause()   { return _pauseSong(mReference); }
    public int resume()  { return _resumeSong(mReference); }
    public boolean isPaused() { return _isSongPaused(mReference); }
    public boolean isDone()   { return _isSongDone(mReference); }

    public int setVolumePercent(int percent) {
        if (percent < 0) percent = 0; if (percent > 100) percent = 100;
        return _setSongVolume(mReference, (int)((percent * 65536L) / 100L));
    }
    public int getVolumePercent() {
        int fixed = _getSongVolume(mReference);
        return fixed <= 0 ? 0 : (int)((fixed * 100L) / 65536L);
    }
    public int getPositionMs() { return _getSongPositionUS(mReference) / 1000; }
    public void seekToMs(int ms) { if (ms < 0) ms = 0; _setSongPositionUS(mReference, ms * 1000); }
    public int getLengthMs()   { return _getSongLengthUS(mReference) / 1000; }
    public int setLoops(int n) { return _setSongLoops(mReference, n); }

    public boolean isPlaying() { return mReference != 0L && !isPaused(); }

    public void close() {
        if (mReference != 0L) {
            stop(true);
            mReference = 0L;
        }
    }
}
