package com.zefie.neobaemc.audio;

/**
 * Wrapper around a NeoBAE sound (one-shot audio file: WAV/MP3/OGG/FLAC/AIFF/AU).
 * Adapted from NeoBAEDroid's Sound.java — the AssetManager loader is removed;
 * sounds load from byte[] or file path.
 */
public class Sound {
    private long mReference;
    Mixer mMixer;

    private native long _newNativeSound(long mixerReference);
    private native int  _loadSoundFromMemory(long soundReference, byte[] data);
    private native int  _loadSoundFromFile(long soundReference, String path);
    private native int  _startSound(long soundReference, int sampleFrames, int fixedVolume);
    private native int  _stopSound(long soundReference, boolean deleteSound);
    private native int  _pauseSound(long soundReference);
    private native int  _resumeSound(long soundReference);
    private native boolean _isSoundPaused(long soundReference);
    private native boolean _isSoundDone(long soundReference);
    private static native int _setSoundVolume(long soundReference, int fixedVolume);
    private static native int _getSoundVolume(long soundReference);
    private static native int _getSoundPositionFrames(long soundReference);
    private static native int _getSoundLengthFrames(long soundReference);
    private static native int _getSoundSampleRate(long soundReference);
    private static native int _setSoundPositionFrames(long soundReference, int sampleFrames);
    private static native int _setSoundLoops(long soundReference, int loopCount);

    Sound(Mixer mixer) {
        mMixer = mixer;
        mReference = _newNativeSound(mMixer.mReference);
    }

    Sound(Mixer mixer, long nativeReference) {
        mMixer = mixer;
        mReference = nativeReference;
    }

    public int loadFromMemory(byte[] data) { return _loadSoundFromMemory(mReference, data); }
    public int loadFromFile(String path)   { return _loadSoundFromFile(mReference, path); }

    public int start() { return start(0); }

    public int start(int sampleFrames) {
        int currentVolume = _getSoundVolume(mReference);
        if (currentVolume == 0 || currentVolume == 0x10000) {
            currentVolume = (int)(2.5 * (1.0 + 1.0) * 65536L);
        }
        int r = _startSound(mReference, sampleFrames, currentVolume);
        if (r == 0 && currentVolume != 0) {
            _setSoundVolume(mReference, currentVolume);
        }
        return r;
    }

    public void stop(boolean deleteSound) { _stopSound(mReference, deleteSound); }
    public int pause()  { return _pauseSound(mReference); }
    public int resume() { return _resumeSound(mReference); }
    public boolean isPaused() { return _isSoundPaused(mReference); }
    public boolean isDone()   { return _isSoundDone(mReference); }

    public int setVolumePercent(int percent) {
        if (percent < 0) percent = 0; if (percent > 100) percent = 100;
        return _setSoundVolume(mReference, (int)((percent * 65536L) / 100L));
    }
    public int getVolumePercent() {
        int fixed = _getSoundVolume(mReference);
        return fixed <= 0 ? 0 : (int)((fixed * 100L) / 65536L);
    }

    public int getPositionMs() {
        int frames = _getSoundPositionFrames(mReference);
        int sr = _getSoundSampleRate(mReference);
        return sr <= 0 ? 0 : (int)((frames * 1000L) / sr);
    }
    public int getLengthMs() {
        int frames = _getSoundLengthFrames(mReference);
        int sr = _getSoundSampleRate(mReference);
        return sr <= 0 ? 0 : (int)((frames * 1000L) / sr);
    }
    public void seekToMs(int ms) {
        if (ms < 0) ms = 0;
        int sr = _getSoundSampleRate(mReference);
        if (sr <= 0) return;
        long framesLong = (ms * (long) sr) / 1000L;
        int len = _getSoundLengthFrames(mReference);
        if (len > 0 && framesLong > len) framesLong = len;
        _setSoundPositionFrames(mReference, (int) framesLong);
    }
    public int setLoops(int n) {
        int loops = (n <= 0) ? 0 : (n >= 32767 ? 0xFFFFFFFF : n);
        return _setSoundLoops(mReference, loops);
    }

    public boolean isPlaying() { return mReference != 0L && !isPaused(); }

    public void close() {
        if (mReference != 0L) {
            stop(true);
            mReference = 0L;
        }
    }
}
