package com.zefie.neobaemc.audio;

import net.minecraft.client.sounds.AudioStream;
import org.lwjgl.BufferUtils;

import javax.sound.sampled.AudioFormat;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Client-side {@link AudioStream} adapter around a buffer of stereo S16LE PCM
 * produced by {@link NeoBAERenderer}.
 *
 * <p>This class is client-only because it implements
 * {@link net.minecraft.client.sounds.AudioStream}. Anything that needs to
 * decode or transcode on the dedicated server must go through
 * {@link NeoBAERenderer} directly.</p>
 */
public final class NeoBAEAudioStream implements AudioStream {

    /** Stereo 16-bit signed little-endian PCM at 44.1 kHz — matches the renderer. */
    public static final AudioFormat FORMAT =
            new AudioFormat(NeoBAERenderer.SAMPLE_RATE, 16, NeoBAERenderer.CHANNELS, true, false);

    private final byte[] pcm;
    private int position;

    private NeoBAEAudioStream(byte[] pcm) {
        this.pcm = pcm;
        this.position = 0;
    }

    /** Forwards to {@link NeoBAERenderer#engineAvailable()}. */
    public static boolean engineAvailable() {
        return NeoBAERenderer.engineAvailable();
    }

    /**
     * Try to decode {@code in} with NeoBAE.
     *
     * @return a new stream on success, or {@code null} if the format wasn't recognised.
     */
    public static NeoBAEAudioStream tryDecode(InputStream in) throws IOException {
        byte[] pcm = NeoBAERenderer.renderToPcm(in);
        if (pcm == null) return null;
        return new NeoBAEAudioStream(pcm);
    }

    @Override
    public AudioFormat getFormat() {
        return FORMAT;
    }

    @Override
    public ByteBuffer read(int amount) throws IOException {
        // amount is in BYTES; align down to a frame boundary.
        int frames = amount / NeoBAERenderer.BYTES_PER_FRAME;
        if (frames <= 0) return BufferUtils.createByteBuffer(0);

        int available = (pcm.length - position) / NeoBAERenderer.BYTES_PER_FRAME;
        int toCopy = Math.min(frames, available);
        if (toCopy <= 0) return BufferUtils.createByteBuffer(0);

        int byteCount = toCopy * NeoBAERenderer.BYTES_PER_FRAME;
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
