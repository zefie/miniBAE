package com.zefie.neobaemc.audio;

import com.zefie.neobaemc.NeoBAEMC;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Side-agnostic NeoBAE render front-end.
 *
 * <p>This class holds the static helpers that drive NeoBAE's mixer to
 * produce PCM (and optionally Vorbis) from an arbitrary input stream.
 * It is intentionally free of {@code net.minecraft.client.*} and
 * {@code org.lwjgl.*} imports so it is safe to load on a dedicated
 * server, where {@link NeoBAEAudioStream} (which implements the
 * client-only {@code AudioStream} interface) cannot be classloaded.</p>
 *
 * <p>All render output is stereo S16LE PCM at {@link #SAMPLE_RATE} Hz.</p>
 */
public final class NeoBAERenderer {

    public static final int SAMPLE_RATE = 44100;
    public static final int CHANNELS = 2;
    public static final int BYTES_PER_FRAME = 4; // 2 ch * 16-bit

    /** Cap on the in-memory copy of a single downloaded file. */
    public static final int MAX_BYTES = 64 * 1024 * 1024;

    /** Safety cap on the duration we will pre-render for one song. */
    public static final int MAX_RENDER_SECONDS = 600;

    /** Frames per renderSamples() call during pre-render. */
    private static final int RENDER_CHUNK_FRAMES = 512;

    /** Serialises access to NeoBAE's singleton mixer for the pre-render phase. */
    static final Object MIXER_LOCK = new Object();

    private static volatile boolean mixerReady = false;

    private NeoBAERenderer() {}

    /** Cheap check used by callers to decide whether to attempt NeoBAE decode. */
    public static boolean engineAvailable() {
        return NativeLoader.ensureLoaded() && ensureMixer();
    }

    public static synchronized boolean ensureMixer() {
        if (mixerReady) return true;
        if (!NativeLoader.ensureLoaded()) return false;
        if (Mixer.exists()) { mixerReady = true; return true; }
        // sampleRate=44100, terpMode=2 (linear), 32 song voices, 8 sound voices, mixLevel=11
        int rc = Mixer.create(SAMPLE_RATE, 2, 32, 8, 11);
        if (rc != 0) {
            NeoBAEMC.LOGGER.error("NeoBAE Mixer.create failed (rc={})", rc);
            return false;
        }
        mixerReady = true;
        return true;
    }

    /**
     * Render the input through NeoBAE and return raw stereo S16LE PCM bytes.
     *
     * @return PCM bytes, or {@code null} if NeoBAE did not recognise the input.
     */
    public static byte[] renderToPcm(InputStream in) throws IOException {
        if (!ensureMixer()) return null;

        byte[] data = readAll(in);
        if (data.length < 4) return null;

        int fileType = Mixer.determineFileTypeByData(data, data.length);
        if (fileType == Mixer.BAE_INVALID_TYPE) return null;

        synchronized (MIXER_LOCK) {
            LoadResult lr = new LoadResult();
            int rc = Mixer.loadFromMemory(data, lr);
            if (rc != 0 || lr.type == LoadResult.BAE_LOAD_TYPE_NONE) {
                NeoBAEMC.LOGGER.warn("NeoBAE recognised format {} but loadFromMemory failed (rc={})",
                        lr.getFileTypeString(), rc);
                return null;
            }

            byte[] pcm;
            try {
                if (lr.isSong()) {
                    Song song = lr.getSong();
                    song.preroll();
                    song.setLoops(0);
                    int srt = song.start();
                    if (srt != 0) NeoBAEMC.LOGGER.warn("Song.start returned {}", srt);
                    pcm = renderUntilDone(song::isDone);
                    song.close();
                } else if (lr.isSound()) {
                    Sound sound = lr.getSound();
                    sound.start();
                    pcm = renderUntilDone(sound::isDone);
                    sound.close();
                } else {
                    return null;
                }
            } finally {
                drainTail();
            }
            NeoBAEMC.LOGGER.info("NeoBAE rendered {} ({} bytes input → {} PCM bytes)",
                    lr.getFileTypeString(), data.length, pcm.length);
            return pcm;
        }
    }

    /**
     * Render input through NeoBAE and encode the result to Ogg/Vorbis.
     * Used by the server cache. Quality is the libvorbis VBR quality
     * (-0.1 .. 1.0); 0.4 ≈ 128 kbps stereo, good for music discs.
     */
    public static byte[] renderToVorbis(InputStream in, float quality) throws IOException {
        byte[] pcm = renderToPcm(in);
        if (pcm == null) return null;
        byte[] ogg = Mixer.encodePcmToVorbis(pcm, SAMPLE_RATE, CHANNELS, quality);
        if (ogg == null) {
            NeoBAEMC.LOGGER.warn("NeoBAE Vorbis encode failed ({} PCM bytes)", pcm.length);
            return null;
        }
        NeoBAEMC.LOGGER.info("NeoBAE transcoded to Vorbis ({} PCM bytes → {} Ogg bytes, ratio {}x)",
                pcm.length, ogg.length, pcm.length / Math.max(1, ogg.length));
        return ogg;
    }

    /** Caller must hold {@link #MIXER_LOCK}. */
    private static byte[] renderUntilDone(java.util.function.BooleanSupplier doneCheck) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(256 * 1024);
        byte[] chunk = new byte[RENDER_CHUNK_FRAMES * BYTES_PER_FRAME];
        int maxFrames = MAX_RENDER_SECONDS * SAMPLE_RATE;
        int producedFrames = 0;
        int tailFrames = 0;
        while (producedFrames < maxFrames) {
            int n = Mixer.renderSamples(chunk, RENDER_CHUNK_FRAMES);
            if (n <= 0) break;
            out.write(chunk, 0, n * BYTES_PER_FRAME);
            producedFrames += n;

            if (doneCheck.getAsBoolean()) {
                tailFrames += n;
                if (tailFrames >= SAMPLE_RATE / 4) break; // ~250 ms reverb tail
            }
        }
        return out.toByteArray();
    }

    /** Caller must hold {@link #MIXER_LOCK}. */
    private static void drainTail() {
        byte[] chunk = new byte[RENDER_CHUNK_FRAMES * BYTES_PER_FRAME];
        for (int i = 0; i < 64; i++) {
            if (!Mixer.isAudioTailActive()) break;
            Mixer.renderSamples(chunk, RENDER_CHUNK_FRAMES);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream baos = new ByteArrayOutputStream(64 * 1024);
        byte[] buf = new byte[8192];
        int n;
        long total = 0;
        while ((n = in.read(buf)) != -1) {
            total += n;
            if (total > MAX_BYTES) {
                throw new IOException("Audio file exceeds NeoBAE in-memory limit of " + MAX_BYTES + " bytes");
            }
            baos.write(buf, 0, n);
        }
        return baos.toByteArray();
    }
}
