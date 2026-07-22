package com.zefie.NeoBAE;
import java.nio.ByteBuffer;
import android.content.res.AssetManager;

public class Sound
{
    // Keep BAESound playback slightly below prior loudness to better match song output.
    private static final int SOUND_GAIN_TRIM_NUM = 9;
    private static final int SOUND_GAIN_TRIM_DEN = 10;

    private long mReference;
    private ByteBuffer mFile;
    Mixer mMixer;

    private static int applyGainTrim(int fixedVolume)
    {
        if (fixedVolume <= 0) return 0;
        long trimmed = (fixedVolume * (long)SOUND_GAIN_TRIM_NUM) / (long)SOUND_GAIN_TRIM_DEN;
        if (trimmed <= 0) return 0;
        if (trimmed > Integer.MAX_VALUE) return Integer.MAX_VALUE;
        return (int)trimmed;
    }

    private native long _newNativeSound(long mixerReference);
    private native int _loadSound(long soundReference, ByteBuffer fileData);
    private native int _loadSound(long soundReference, AssetManager assetManager, String file);
    private native int _startSound(long soundReference, int sampleFrames, int fixedVolume);
    private native int _stopSound(long soundReference, boolean deleteSound);
    private native int _pauseSound(long soundReference);
    private native int _resumeSound(long soundReference);
    private native boolean _isSoundPaused(long soundReference);
    private native boolean _isSoundDone(long soundReference);
    private static native int _setSoundVolume(long soundReference, int fixedVolume);
    private static native int _getSoundVolume(long soundReference);
    private static native int _getSoundPositionFrames(long soundReference);
    private static native int _getSoundLengthFrames(long soundReference);
    private static native int _getSoundSampleRate(long soundReference);
    private static native int _setSoundPositionFrames(long soundReference, int sampleFrames);
    private static native int _setSoundLoops(long soundReference, int loopCount);

	Sound(Mixer mixer)
	{
        mMixer = mixer;
		mReference = _newNativeSound(mMixer.mReference);
        if (mReference == 0L)
		{
            // good
		}
        else
        {
            // bad
        }
	}
	
	// Package-private constructor for LoadResult to wrap existing native sound
	Sound(Mixer mixer, long nativeReference)
	{
		mMixer = mixer;
		mReference = nativeReference;
	}

    int load(String resourceName)
    {
        mFile = ByteBuffer.allocateDirect(10000);
        int status = _loadSound(mReference, mMixer.mAssetManager, resourceName);
        return status;
    }

    int load()
    {
        mFile = ByteBuffer.allocateDirect(10000);
        int status = _loadSound(mReference, mFile);
        return status;
    }
    
    public int start()
    {
        return start(0);
    }
    
    public int start(int sampleFrames)
    {
        // Use current Sound volume. If native reports 0/unset, fall back to unity.
        int currentVolume = _getSoundVolume(mReference);
        if (currentVolume <= 0) {
            currentVolume = 0x10000;
        }
        int startVolume = applyGainTrim(currentVolume);
        int r = _startSound(mReference, sampleFrames, startVolume);
        // Apply volume again immediately after starting to ensure it persists
        if (r == 0 && startVolume != 0) {
            _setSoundVolume(mReference, startVolume);
        }
        return r;
    }

    public void stop(boolean deleteSound)
    {
        _stopSound(mReference, deleteSound);
    }

    public int pause()
    {
        return _pauseSound(mReference);
    }

    public int resume()
    {
        return _resumeSound(mReference);
    }

    public boolean isPaused()
    {
        return _isSoundPaused(mReference);
    }

    public boolean isDone()
    {
        return _isSoundDone(mReference);
    }

    public int setVolumePercent(int percent){
        if(percent<0) percent=0; if(percent>100) percent=100;
        // 16.16 fixed where 1.0 == 65536. Map 0..100% -> 0..1.0.
        int fixed = (int)((percent * 65536L) / 100L);
        fixed = applyGainTrim(fixed);
        return _setSoundVolume(mReference, fixed);
    }
    
    public int getVolumePercent(){ 
        int fixed = _getSoundVolume(mReference); // fixed 16.16
        if(fixed <= 0) return 0;
        // Undo internal trim so UI/user-facing percent remains intuitive.
        long untrimmed = (fixed * (long)SOUND_GAIN_TRIM_DEN) / (long)SOUND_GAIN_TRIM_NUM;
        int percent = (int)((untrimmed * 100L) / 65536L);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        return percent;
    }
    
    public int getPositionMs() {
        int frames = _getSoundPositionFrames(mReference);
        int sampleRate = _getSoundSampleRate(mReference);
        if (sampleRate <= 0) return 0;
        return (int)((frames * 1000L) / sampleRate);
    }
    
    public int getLengthMs() {
        int frames = _getSoundLengthFrames(mReference);
        int sampleRate = _getSoundSampleRate(mReference);
        if (sampleRate <= 0) return 0;
        return (int)((frames * 1000L) / sampleRate);
    }

    public void seekToMs(int ms) {
        if (ms < 0) ms = 0;
        int sampleRate = _getSoundSampleRate(mReference);
        if (sampleRate <= 0) return;
        long framesLong = (ms * (long)sampleRate) / 1000L;
        int lengthFrames = _getSoundLengthFrames(mReference);
        if (lengthFrames > 0) {
            if (framesLong > (long)lengthFrames) framesLong = (long)lengthFrames;
        }
        if (framesLong < 0) framesLong = 0;
        _setSoundPositionFrames(mReference, (int)framesLong);
    }

    // Loop control for sounds (0 = none, positive = that many, 0xFFFFFFFF (-1) = infinite)
    public int setLoops(int numLoops) {
        int loops;
        if (numLoops <= 0) {
            loops = 0;
        } else if (numLoops >= 32767) {
            loops = 0xFFFFFFFF;
        } else {
            loops = numLoops;
        }
        return _setSoundLoops(mReference, loops);
    }
    
    public boolean isPlaying() {
        return !isPaused(); // If not paused, assume it's playing
    }
    
    public void close() {
        // Stop sound if playing
        if (mReference != 0L) {
            stop(true);
            mReference = 0L;
        }
    }
}
