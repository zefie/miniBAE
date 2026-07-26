package com.zefie.neobaemc.client;

import com.zefie.neobaemc.NeoBAEMC;

import java.io.IOException;
import java.io.InputStream;
import java.io.PipedInputStream;
import java.io.PipedOutputStream;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Per-JVM registry of in-progress server-streamed sound downloads.
 *
 * <p>For each request the client makes via
 * {@link com.zefie.neobaemc.net.SoundRequestPayload}, an entry is added
 * here. As {@link com.zefie.neobaemc.net.SoundChunkPayload}s arrive on the
 * network thread, they are appended to a pipe whose read end is fed to
 * a vanilla {@code OggAudioStream}, so playback streams as bytes arrive.</p>
 *
 * <p>The pipe size is intentionally large so the network thread never
 * blocks waiting for the decoder; we'd rather buffer one full song worth
 * of compressed bytes (~5 MB worst case) in RAM than stall I/O.</p>
 */
public final class ClientStreamRegistry {

    /** ~6 MB - enough to buffer a typical music disc end-to-end if the decoder is slow. */
    private static final int PIPE_CAPACITY = 6 * 1024 * 1024;

    private static final AtomicInteger NEXT_ID = new AtomicInteger(1);
    private static final Map<Integer, Pending> ACTIVE = new ConcurrentHashMap<>();

    private ClientStreamRegistry() {}

    /** A single pending streaming download. */
    public static final class Pending {
        public final int streamId;
        public final PipedInputStream in;
        public final PipedOutputStream out;
        public final CompletableFuture<Void> doneFuture = new CompletableFuture<>();
        public volatile int receivedBytes = 0;
        public volatile int totalBytes = -1; // unknown until first chunk arrives
        public volatile boolean closed = false;

        private Pending(int id) throws IOException {
            this.streamId = id;
            this.in  = new PipedInputStream(PIPE_CAPACITY);
            this.out = new PipedOutputStream(this.in);
        }

        public InputStream getInputStream() { return in; }
    }

    /** Allocate a new pending stream and register it. */
    public static Pending open() throws IOException {
        int id = NEXT_ID.getAndIncrement();
        Pending p = new Pending(id);
        ACTIVE.put(id, p);
        return p;
    }

    /** Called by the network handler when a chunk arrives. */
    public static void onChunk(int streamId, int totalSize, byte[] data) {
        Pending p = ACTIVE.get(streamId);
        if (p == null) {
            NeoBAEMC.LOGGER.warn("NeoBAE chunk for unknown stream id {} ({} bytes)", streamId, data.length);
            return;
        }
        p.totalBytes = totalSize;
        try {
            p.out.write(data);
            p.receivedBytes += data.length;
            if (p.receivedBytes >= totalSize) {
                p.out.close();
                p.closed = true;
                p.doneFuture.complete(null);
                ACTIVE.remove(streamId);
            }
        } catch (IOException e) {
            NeoBAEMC.LOGGER.warn("NeoBAE pipe write failed for stream {}", streamId, e);
            cancel(streamId, e);
        }
    }

    /** Called by the network handler when the server reports an error. */
    public static void onError(int streamId, String message) {
        Pending p = ACTIVE.remove(streamId);
        if (p == null) return;
        try { p.out.close(); } catch (IOException ignored) {}
        p.closed = true;
        p.doneFuture.completeExceptionally(new IOException("server: " + message));
    }

    /** Mark a stream as cancelled (decoder gave up, etc.). */
    public static void cancel(int streamId, Throwable cause) {
        Pending p = ACTIVE.remove(streamId);
        if (p == null) return;
        try { p.out.close(); } catch (IOException ignored) {}
        p.closed = true;
        p.doneFuture.completeExceptionally(cause);
    }
}
