package com.zefie.neobaemc.audio;

public class LoadResult {
    public static final int BAE_LOAD_TYPE_NONE  = 0;
    public static final int BAE_LOAD_TYPE_SONG  = 1;
    public static final int BAE_LOAD_TYPE_SOUND = 2;

    public int  type = BAE_LOAD_TYPE_NONE;
    public int  fileType = Mixer.BAE_INVALID_TYPE;
    public int  result = 0;
    public long songReference = 0;
    public long soundReference = 0;

    private Song song;
    private Sound sound;
    private Mixer mixer;

    public LoadResult() {}

    public Song getSong() {
        if (song == null && type == BAE_LOAD_TYPE_SONG && songReference != 0) {
            song = new Song(mixer, songReference);
        }
        return song;
    }
    public Sound getSound() {
        if (sound == null && type == BAE_LOAD_TYPE_SOUND && soundReference != 0) {
            sound = new Sound(mixer, soundReference);
        }
        return sound;
    }

    public boolean isSong()  { return type == BAE_LOAD_TYPE_SONG; }
    public boolean isSound() { return type == BAE_LOAD_TYPE_SOUND; }

    public String getFileTypeString() {
        return switch (fileType) {
            case Mixer.BAE_MIDI_TYPE   -> "MIDI";
            case Mixer.BAE_RMF         -> "RMF";
            case Mixer.BAE_RMI         -> "RMI";
            case Mixer.BAE_AIFF_TYPE   -> "AIFF";
            case Mixer.BAE_WAVE_TYPE   -> "WAVE";
            case Mixer.BAE_AU_TYPE     -> "AU";
            case Mixer.BAE_MPEG_TYPE   -> "MP3";
            case Mixer.BAE_FLAC_TYPE   -> "FLAC";
            case Mixer.BAE_VORBIS_TYPE -> "OGG Vorbis";
            case Mixer.BAE_OPUS_TYPE   -> "Opus";
            case Mixer.BAE_GROOVOID    -> "Groovoid";
            case Mixer.BAE_XMF         -> "XMF";
            case Mixer.BAE_MTHC        -> "Nokia Compressed MIDI";
            case Mixer.BAE_ADP_TYPE    -> "Nokia ADP";
            case Mixer.BAE_RAW_PCM     -> "Raw PCM";
            case Mixer.BAE_ADX_TYPE    -> "CRI ADX";
            default -> "Unknown";
        };
    }

    public void cleanup() {
        if (song != null) { song.close(); song = null; }
        sound = null;
        songReference = 0;
        soundReference = 0;
        type = BAE_LOAD_TYPE_NONE;
    }

    void setMixer(Mixer m) { this.mixer = m; }
}
