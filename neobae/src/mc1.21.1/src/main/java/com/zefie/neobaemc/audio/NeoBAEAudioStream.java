package com.zefie.neobaemc.audio;

import com.zefie.neobaemc.NeoBAEMC;
import net.minecraft.client.sounds.AudioStream;
import org.lwjgl.BufferUtils;

import javax.sound.sampled.AudioFormat;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Bridges NeoBAE to Minecraft's {@link AudioStream} contract.
 *
 * <p>{@link #tryDecode} reads the entire input into memory, asks NeoBAE
 * what format it is, and — if recognised — pre-renders the entire song /
 * sound to a stereo S16LE PCM byte array under a global lock that
 * serialises access to NeoBAE's singleton mixer. The returned
 * {@code NeoBAEAudioStream} then hands out PCM slices to Minecraft from
 * that buffer with no engine state, so simultaneous playback of multiple
 * NeoBAE-decoded streams (e.g. several music discs) works the same way
 * any other Minecraft sound does — each gets its own buffer, OpenAL does
 * the 3D positioning and per-source volume.</p>
 *
 * <p>Trade-off: each decoded song occupies ~10 MB / minute of RAM. The
 * cap is {@link #MAX_RENDER_SECONDS} (default 10 min).</p>
 */
public final class NeoBAEAudioStream implements AudioStream {

    /** Stereo 16-bit signed little-endian PCM at 44.1 kHz. */
    public static final AudioFormat FORMAT =
            new AudioFormat(44100f, 16, 2, true, false);

    /** Cap on the in-memory copy of a single downloaded file. */
    public static final int MAX_BYTES = 64 * 1024 * 1024;

    /** Safety cap on the duration we will pre-render for one song. */
    public static final int MAX_RENDER_SECONDS = 600;

    /** Frames per renderSamples() call during pre-render. Matches the engine slice size. */
    private static final int RENDER_CHUNK_FRAMES = 512;

    /** Bytes per frame for the output format. */
    private static final int BYTES_PER_FRAME = 4;

    /** Serialises access to NeoBAE's singleton mixer for the pre-render phase. */
    private static final Object MIXER_LOCK = new Object();

    private static volatile boolean mixerReady = false;

    private final byte[] pcm;
    private int position;

    private NeoBAEAudioStream(byte[] pcm) {
        this.pcm = pcm;
        this.position = 0;
    }

    /** Cheap check used by the mixin to decide whether to attempt override at all. */
    public static boolean engineAvailable() {
        return NativeLoader.ensureLoaded() && ensureMixer();
    }

    public static synchronized boolean ensureMixer() {
        if (mixerReady) return true;
        if (!NativeLoader.ensureLoaded()) return false;
        if (Mixer.exists()) { mixerReady = true; return true; }
        // sampleRate=44100, terpMode=2 (linear), 32 song voices, 8 sound voices, mixLevel=11
        int rc = Mixer.create(44100, 2, 32, 8, 11);
        if (rc != 0) {
            NeoBAEMC.LOGGER.error("NeoBAE Mixer.create failed (rc={})", rc);
            return false;
        }
        mixerReady = true;
        return true;
    }

    /**
     * Try to decode {@code in} with NeoBAE.
     *
     * <p>If NeoBAE recognises the format, this pre-renders the entire decoded
     * stream into a PCM byte array under {@link #MIXER_LOCK} (so two concurrent
     * disc loads don't fight over the singleton mixer), then returns a stream
     * that just slices through that array — fully independent of the mixer
     * thereafter.</p>
     *
     * @return a {@link NeoBAEAudioStream} on success, or {@code null} if
     *         NeoBAE does not recognise the file (caller should fall back).
     * @throws IOException on I/O failure reading {@code in}.
     */
    public static NeoBAEAudioStream tryDecode(InputStream in) throws IOException {
        if (!ensureMixer()) return null;

        byte[] data = readAll(in);
        if (data.length < 4) return null;

        int fileType = Mixer.determineFileTypeByData(data, data.length);
        if (fileType == Mixer.BAE_INVALID_TYPE) return null;

        // Hold the mixer lock for the entire load+render+unload cycle.
        // The engine is a singleton; one render at a time.
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
                    if (srt != 0) {
                        NeoBAEMC.LOGGER.warn("Song.start returned {}", srt);
                    }
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
                // Flush any reverb tail so the next decode starts from silence.
                drainTail();
            }

            NeoBAEMC.LOGGER.info("NeoBAE pre-rendered {} ({} bytes input → {} PCM bytes)",
                    lr.getFileTypeString(), data.length, pcm.length);
            return new NeoBAEAudioStream(pcm);
        }
    }

    /** Caller must hold {@link #MIXER_LOCK}. */
    private static byte[] renderUntilDone(java.util.function.BooleanSupplier doneCheck) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(256 * 1024);
        byte[] chunk = new byte[RENDER_CHUNK_FRAMES * BYTES_PER_FRAME];
        int maxFrames = MAX_RENDER_SECONDS * (int) FORMAT.getSampleRate();
        int producedFrames = 0;
        // Keep going as long as the song reports not-done, plus a short "tail" window
        // after isDone() flips so reverb decay is captured.
        int tailFrames = 0;
        while (producedFrames < maxFrames) {
            int n = Mixer.renderSamples(chunk, RENDER_CHUNK_FRAMES);
            if (n <= 0) break;
            out.write(chunk, 0, n * BYTES_PER_FRAME);
            producedFrames += n;

            if (doneCheck.getAsBoolean()) {
                tailFrames += n;
                // ~250 ms of post-done audio for reverb tail
                if (tailFrames >= (int) (FORMAT.getSampleRate() / 4)) break;
            }
        }
        return out.toByteArray();
    }

    /** Caller must hold {@link #MIXER_LOCK}. Renders silence slices until the
     *  mixer reports no remaining audio tail, capped so we never hang. */
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

    @Override
    public AudioFormat getFormat() {
        return FORMAT;
    }

    @Override
    public ByteBuffer read(int amount) throws IOException {
        // amount is in BYTES; align down to a frame boundary.
        int frames = amount / BYTES_PER_FRAME;
        if (frames <= 0) return BufferUtils.createByteBuffer(0);

        int available = (pcm.length - position) / BYTES_PER_FRAME;
        int toCopy = Math.min(frames, available);
        if (toCopy <= 0) return BufferUtils.createByteBuffer(0);

        int byteCount = toCopy * BYTES_PER_FRAME;
        ByteBuffer out = BufferUtils.createByteBuffer(byteCount).order(ByteOrder.nativeOrder());
        out.put(pcm, position, byteCount);
        out.flip();
        position += byteCount;
        return out;
    }

    @Override
    public void close() {
        // Pre-rendered PCM lives in a byte[]; nothing native to release.
    }
}
