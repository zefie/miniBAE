package com.zefie.neobaemc.audio;

/**
 * Java wrapper around the NeoBAE mixer.
 *
 * <p>Adapted from the NeoBAEDroid Mixer.java - the AssetManager-only paths are
 * removed; everything goes through file paths or {@code byte[]} buffers
 * suitable for the desktop / Minecraft environment.
 *
 * <p>This class also exposes a new pull-mode native method
 * {@link #_renderSamples} that the platform Pull backend uses to give a chunk
 * of stereo 16-bit PCM at the configured sample rate. Minecraft's OpenAL
 * streaming source feeds this into an audio device.
 */
public class Mixer {
    long mReference;

    private static Mixer mMixer;

    static {
        NativeLoader.ensureLoaded();
    }

    // -- Lifecycle -----------------------------------------------------------
    private static native long _newMixer();
    private static native void _deleteMixer(long reference);
    private static native int  _openMixer(long reference, int sampleRate, int terpMode, int maxSongVoices, int maxSoundVoices, int mixLevel);
    private static native int  _disengageAudio(long reference);
    private static native int  _reengageAudio(long reference);
    private static native int  _isAudioEngaged(long reference);
    private static native int  _isAudioTailActive(long reference);

    // -- Settings ------------------------------------------------------------
    private static native int  _setDefaultReverb(long reference, int reverbType);
    private static native int  _getActiveVoiceCount(long reference);
    private static native int  _addBankFromFile(long reference, String path);
    private static native int  _addBankFromMemory(long reference, byte[] data);
    private static native int  _addBankFromMemoryWithFilename(long reference, byte[] data, String filename);
    private static native int  _setMasterVolume(long reference, int fixedVolume);
    private static native int  _setGlobalVolume(long reference, int fixedVolume);
    private static native int  _getGlobalVolume(long reference);
    private static native String _getBankFriendlyName(long reference);
    private static native String _getVersion();
    private static native String _getCompileInfo();
    private static native String _getFeatureString();
    private static native int  _determineFileTypeByData(byte[] data, int length);
    private static native int  _loadFromMemory(long mixerReference, byte[] data, LoadResult result);

    /**
     * Encode interleaved S16LE PCM into an Ogg/Vorbis blob (in-memory).
     * Returns the encoded bytes, or {@code null} on encoder failure.
     */
    private static native byte[] _encodePcmToVorbis(byte[] pcmS16LE, int sampleRate, int channels, float quality);

    // -- Pull-mode rendering (new - for Minecraft sound integration) ---------
    /**
     * Render {@code sampleFrames} stereo 16-bit PCM frames into {@code out}
     * (interleaved L,R,L,R...). The byte buffer must be at least
     * {@code sampleFrames * 4} bytes long.
     *
     * @return number of frames actually produced (typically equal to request)
     */
    private static native int _renderSamples(long reference, byte[] out, int sampleFrames);

    private Mixer() {
        mReference = _newMixer();
    }

    /** Create the global mixer. {@code sampleRate} typical: 44100 or 48000. */
    public static int create(int sampleRate, int terpMode, int maxSongVoices, int maxSoundVoices, int mixLevel) {
        int status = 0;
        if (mMixer == null) {
            mMixer = new Mixer();
        }
        if (mMixer.mReference != 0L) {
            status = _openMixer(mMixer.mReference, sampleRate, terpMode, maxSongVoices, maxSoundVoices, mixLevel);
        }
        return status;
    }

    public static void delete() {
        if (mMixer != null && mMixer.mReference != 0L) {
            _deleteMixer(mMixer.mReference);
            mMixer.mReference = 0L;
            mMixer = null;
        }
    }

    public static boolean exists() { return (mMixer != null && mMixer.mReference != 0L); }

    public static int disengageAudio() { return mMixer == null ? -1 : _disengageAudio(mMixer.mReference); }
    public static int reengageAudio()  { return mMixer == null ? -1 : _reengageAudio(mMixer.mReference); }
    public static boolean isAudioEngaged()    { return mMixer != null && _isAudioEngaged(mMixer.mReference) != 0; }
    public static boolean isAudioTailActive() { return mMixer != null && _isAudioTailActive(mMixer.mReference) != 0; }

    public static int setDefaultReverb(int reverbType) { return mMixer == null ? -1 : _setDefaultReverb(mMixer.mReference, reverbType); }
    public static int getActiveVoiceCount()            { return mMixer == null ? 0  : _getActiveVoiceCount(mMixer.mReference); }
    public static int addBankFromFile(String path)     { return mMixer == null ? -1 : _addBankFromFile(mMixer.mReference, path); }
    public static int addBankFromMemory(byte[] data)   { return mMixer == null ? -1 : _addBankFromMemory(mMixer.mReference, data); }
    public static int addBankFromMemory(byte[] data, String filename) { return mMixer == null ? -1 : _addBankFromMemoryWithFilename(mMixer.mReference, data, filename); }

    public static int setMasterVolumePercent(int percent) {
        if (mMixer == null) return -1;
        if (percent < 0) percent = 0; if (percent > 100) percent = 100;
        int fixed = (int) ((percent * 65536L) / 100L);
        return _setMasterVolume(mMixer.mReference, fixed);
    }

    public static int setGlobalVolumePercent(int percent) {
        if (mMixer == null) return -1;
        if (percent < 0) percent = 0; if (percent > 100) percent = 100;
        int fixed = (int) ((percent * 65536L) / 100L);
        return _setGlobalVolume(mMixer.mReference, fixed);
    }

    public static String getBankFriendlyName() { return mMixer == null ? null : _getBankFriendlyName(mMixer.mReference); }
    public static String getVersion()      { return _getVersion(); }
    public static String getCompileInfo()  { return _getCompileInfo(); }
    public static String getFeatureString(){ return _getFeatureString(); }

    public static int determineFileTypeByData(byte[] data, int length) {
        if (data == null || length <= 0) return BAE_INVALID_TYPE;
        return _determineFileTypeByData(data, length);
    }

    public static int loadFromMemory(byte[] data, LoadResult result) {
        if (mMixer == null || data == null || result == null) return -1;
        result.setMixer(mMixer);
        return _loadFromMemory(mMixer.mReference, data, result);
    }

    /**
     * Encode 16-bit signed little-endian interleaved PCM to an Ogg/Vorbis
     * blob. {@code quality} is libvorbis's VBR quality (-0.1 .. 1.0); 0.4
     * is a sensible default for music (~128 kbps stereo).
     *
     * @return the encoded ogg bytes, or {@code null} on failure.
     */
    public static byte[] encodePcmToVorbis(byte[] pcmS16LE, int sampleRate, int channels, float quality) {
        if (pcmS16LE == null || pcmS16LE.length == 0) return null;
        if (channels < 1 || channels > 2) return null;
        return _encodePcmToVorbis(pcmS16LE, sampleRate, channels, quality);
    }

    /** Render PCM into {@code out}. See {@link #_renderSamples}. */
    public static int renderSamples(byte[] out, int sampleFrames) {
        if (mMixer == null || mMixer.mReference == 0L) return 0;
        return _renderSamples(mMixer.mReference, out, sampleFrames);
    }

    public static Mixer getMixer() { return mMixer; }

    // -- BAEFileType constants ----------------------------------------------
    public static final int BAE_INVALID_TYPE = 0;
    public static final int BAE_AIFF_TYPE   = 1;
    public static final int BAE_WAVE_TYPE   = 2;
    public static final int BAE_MPEG_TYPE   = 3;
    public static final int BAE_AU_TYPE     = 4;
    public static final int BAE_MIDI_TYPE   = 5;
    public static final int BAE_FLAC_TYPE   = 6;
    public static final int BAE_VORBIS_TYPE = 7;
    public static final int BAE_OPUS_TYPE   = 8;
    public static final int BAE_GROOVOID    = 9;
    public static final int BAE_RMF         = 10;
    public static final int BAE_XMF         = 11;
    public static final int BAE_MTHC        = 12;
    public static final int BAE_RMI         = 13;
    public static final int BAE_ADP_TYPE    = 14;
    public static final int BAE_RAW_PCM     = 15;
    public static final int BAE_ADX_TYPE    = 16;
}
