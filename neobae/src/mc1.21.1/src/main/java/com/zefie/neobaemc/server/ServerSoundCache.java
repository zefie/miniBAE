package com.zefie.neobaemc.server;

import com.zefie.neobaemc.NeoBAEMC;
import com.zefie.neobaemc.audio.NeoBAERenderer;
import com.zefie.neobaemc.net.SoundChunkPayload;
import com.zefie.neobaemc.net.SoundErrorPayload;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.neoforged.neoforge.network.PacketDistributor;

import java.io.BufferedInputStream;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Server-side sound cache + transcoder.
 *
 * <p>For each remote URL the client requests, this:
 * <ol>
 *   <li>Looks up a cached Ogg/Vorbis blob on disk under
 *       {@code <world>/neobaemc-cache/<sha1>.ogg}.</li>
 *   <li>If absent, downloads the original URL, runs it through NeoBAE
 *       (RMF/MIDI/MP3/FLAC/...), encodes the rendered PCM to Vorbis, and
 *       writes the result to the cache file.</li>
 *   <li>Streams the cached bytes to the requesting client in
 *       {@code CHUNK_SIZE}-byte chunks via {@link SoundChunkPayload}.</li>
 * </ol>
 *
 * <p>The original URL only has to be reachable once — after the server has
 * transcoded it the world save owns a permanent copy.</p>
 *
 * <p>Concurrent requests for the same URL are coalesced into a single
 * download/transcode (everyone subscribes to the same future).</p>
 */
public final class ServerSoundCache {

    /** Vanilla NeoForge custom-payload soft cap is ~1 MB; stay well under it. */
    private static final int CHUNK_SIZE = 96 * 1024;

    /** libvorbis VBR quality. 0.4 ≈ 128 kbps stereo, good for music. */
    private static final float VORBIS_QUALITY = 0.4f;

    /** Hard cap on the source download (raw, pre-transcode). */
    private static final int MAX_DOWNLOAD_BYTES = 64 * 1024 * 1024;

    /** Dedicated thread pool — downloads + Vorbis encode are blocking and CPU-heavy. */
    private static final ExecutorService EXECUTOR =
            Executors.newFixedThreadPool(Math.max(1, Runtime.getRuntime().availableProcessors() / 4),
                    r -> {
                        Thread t = new Thread(r, "neobaemc-cache");
                        t.setDaemon(true);
                        return t;
                    });

    private static volatile MinecraftServer server;
    private static volatile Path cacheDir;

    /** url → in-flight or completed cache file. */
    private static final Map<String, CompletableFuture<Path>> inFlight = new ConcurrentHashMap<>();

    private ServerSoundCache() {}

    /** Called from {@code ServerStartedEvent} / {@code ServerStoppingEvent}. */
    public static void onServerStarted(MinecraftServer s) {
        server = s;
        try {
            cacheDir = s.getWorldPath(net.minecraft.world.level.storage.LevelResource.ROOT)
                    .resolve("neobaemc-cache");
            Files.createDirectories(cacheDir);
            NeoBAEMC.LOGGER.info("NeoBAE sound cache: {}", cacheDir);
        } catch (IOException e) {
            NeoBAEMC.LOGGER.error("Could not create NeoBAE sound cache dir", e);
            cacheDir = null;
        }
    }

    public static void onServerStopping(MinecraftServer s) {
        server = null;
        cacheDir = null;
        inFlight.clear();
    }

    /** Handler for {@link com.zefie.neobaemc.net.SoundRequestPayload} on the server. */
    public static void handleRequest(int streamId, String url, ServerPlayer player) {
        if (cacheDir == null) {
            sendError(player, streamId, "server cache not ready");
            return;
        }
        if (url == null || url.isEmpty()) {
            sendError(player, streamId, "empty URL");
            return;
        }
        // Coalesce: if another player already triggered this download, ride along.
        CompletableFuture<Path> future = inFlight.computeIfAbsent(url, u ->
                CompletableFuture.supplyAsync(() -> resolveCached(u), EXECUTOR));

        future.whenCompleteAsync((path, throwable) -> {
            if (throwable != null) {
                NeoBAEMC.LOGGER.warn("NeoBAE cache resolve failed for {}", url, throwable);
                sendError(player, streamId, throwable.getMessage() != null ? throwable.getMessage() : "transcode failed");
                inFlight.remove(url); // allow retry
                return;
            }
            try {
                streamCachedFile(player, streamId, path);
            } catch (IOException ioe) {
                NeoBAEMC.LOGGER.warn("NeoBAE chunk-send failed for {}", url, ioe);
                sendError(player, streamId, "send failed: " + ioe.getMessage());
            }
        }, EXECUTOR);
    }

    /** Returns the cache file path, creating it (download + transcode) on miss. */
    private static Path resolveCached(String url) {
        Path target = cacheDir.resolve(sha1(url) + ".ogg");
        if (Files.isRegularFile(target) && sizeSafe(target) > 0) {
            return target;
        }
        byte[] downloaded = download(url);
        byte[] ogg;
        try (var in = new java.io.ByteArrayInputStream(downloaded)) {
            ogg = NeoBAERenderer.renderToVorbis(in, VORBIS_QUALITY);
        } catch (IOException e) {
            throw new RuntimeException("NeoBAE decode failed: " + e.getMessage(), e);
        }
        if (ogg == null) {
            throw new RuntimeException("NeoBAE did not recognise or could not transcode " + url);
        }
        try {
            // Write atomically — temp file + move — so a crash mid-write can't poison the cache.
            Path tmp = target.resolveSibling(target.getFileName() + ".tmp");
            Files.write(tmp, ogg);
            Files.move(tmp, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException e) {
            throw new RuntimeException("Could not write cache file " + target + ": " + e.getMessage(), e);
        }
        return target;
    }

    private static byte[] download(String url) {
        try {
            URL u = new URI(url).toURL();
            HttpURLConnection conn = (HttpURLConnection) u.openConnection();
            // Mirror Etched's headers so servers that gate on user-agent are happy.
            conn.setRequestProperty("User-Agent", "NeoBAE-MC/1.0 (+etched)");
            conn.setInstanceFollowRedirects(true);
            conn.setConnectTimeout(15_000);
            conn.setReadTimeout(60_000);
            int rc = conn.getResponseCode();
            if (rc != 200) {
                throw new IOException("HTTP " + rc + " " + conn.getResponseMessage());
            }
            try (var in = new BufferedInputStream(conn.getInputStream())) {
                var baos = new java.io.ByteArrayOutputStream(64 * 1024);
                byte[] buf = new byte[8192];
                int n; long total = 0;
                while ((n = in.read(buf)) != -1) {
                    total += n;
                    if (total > MAX_DOWNLOAD_BYTES) {
                        throw new IOException("download exceeds " + MAX_DOWNLOAD_BYTES + " bytes");
                    }
                    baos.write(buf, 0, n);
                }
                return baos.toByteArray();
            }
        } catch (Exception e) {
            throw new RuntimeException("download " + url + " failed: " + e.getMessage(), e);
        }
    }

    private static void streamCachedFile(ServerPlayer player, int streamId, Path file) throws IOException {
        byte[] all = Files.readAllBytes(file);
        int totalSize = all.length;
        int offset = 0;
        while (offset < totalSize) {
            int n = Math.min(CHUNK_SIZE, totalSize - offset);
            byte[] chunk = new byte[n];
            System.arraycopy(all, offset, chunk, 0, n);
            PacketDistributor.sendToPlayer(player, new SoundChunkPayload(streamId, totalSize, chunk));
            offset += n;
        }
    }

    private static void sendError(ServerPlayer player, int streamId, String message) {
        PacketDistributor.sendToPlayer(player, new SoundErrorPayload(streamId, message));
    }

    private static String sha1(String s) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            return HexFormat.of().formatHex(md.digest(s.getBytes(java.nio.charset.StandardCharsets.UTF_8)));
        } catch (Exception e) {
            // SHA-1 is mandatory in every JDK; this can't really fail.
            return Integer.toHexString(s.hashCode());
        }
    }

    private static long sizeSafe(Path p) {
        try { return Files.size(p); } catch (IOException e) { return 0; }
    }
}
