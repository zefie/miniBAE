package com.zefie.neobaemc.mixin;

import com.google.common.collect.ImmutableSet;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Pseudo;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

import java.net.HttpURLConnection;
import java.util.Set;

/**
 * Etched's {@code EtchingMenu#checkStatus} rejects URLs whose {@code Content-Type}
 * isn't in a hard-coded allow-list ({@code audio/wav, audio/mpeg, audio/ogg, ...}).
 * Since NeoBAE can decode FLAC, MIDI, RMF, XMF, AIFF and more, we widen that
 * allow-list by redirecting the single {@code Set.contains(Object)} call in
 * {@code checkStatus} to also accept our extra MIME types.
 *
 * <p>{@link Pseudo @Pseudo} keeps the mixin a no-op (rather than a hard error)
 * when Etched is absent.
 */
@Pseudo
@Mixin(targets = "gg.moonflower.etched.common.menu.EtchingMenu", remap = false)
public abstract class EtchingMenuMixin {

    /** Extra Content-Type values that NeoBAE handles natively. */
    private static final Set<String> NEOBAE_EXTRA_FORMATS = ImmutableSet.of(
            // MIDI variants
            "audio/midi",
            "audio/x-midi",
            "audio/mid",
            "application/x-midi",
            // FLAC
            "audio/flac",
            "audio/x-flac",
            // AIFF
            "audio/aiff",
            "audio/x-aiff",
            // Beatnik / Rich Music Format and XMF
            "audio/rmf",
            "audio/x-rmf",
            "audio/mobile-xmf",
            "audio/xmf",
            "application/vnd.rn-rmf",
            // MOD-family trackers (decoded by miniBAE's MOD support)
            "audio/x-mod",
            "audio/mod",
            // Au / SND
            "audio/basic",
            "audio/au",
            // ZMF (RMF with modern codecs) - rarely has a registered MIME
            "audio/zmf",
            "audio/x-zmf",
            // Generic octet-stream / unknown - NeoBAE will sniff the bytes
            "application/octet-stream",
            "binary/octet-stream"
    );

    @Redirect(
            method = "checkStatus(Ljava/lang/String;)V",
            at = @At(
                    value = "INVOKE",
                    target = "Ljava/util/Set;contains(Ljava/lang/Object;)Z",
                    remap = false
            ),
            remap = false
    )
    private static boolean neobaemc$acceptExtraContentTypes(Set<String> validFormats, Object key) {
        // Some servers don't set Content-Type for .zmf / .rmf / etc. and Etched
        // calls Set.contains(null), which ImmutableSet rejects with NPE. Treat
        // a missing content type as "let NeoBAE try to sniff it".
        if (key == null) return true;
        if (validFormats.contains(key)) return true;
        return key instanceof String s && NEOBAE_EXTRA_FORMATS.contains(s);
    }

    /**
     * Etched calls {@code connection.getContentType().toLowerCase(Locale.ROOT)}
     * with no null check. Servers hosting {@code .zmf} / {@code .rmf} files
     * often omit {@code Content-Type} entirely, which throws NPE before our
     * {@link #neobaemc$acceptExtraContentTypes} redirect ever runs. Substitute
     * a safe placeholder so the call chain continues and our other redirect
     * accepts it.
     */
    @Redirect(
            method = "checkStatus(Ljava/lang/String;)V",
            at = @At(
                    value = "INVOKE",
                    target = "Ljava/net/HttpURLConnection;getContentType()Ljava/lang/String;",
                    remap = false
            ),
            remap = false
    )
    private static String neobaemc$safeContentType(HttpURLConnection conn) {
        String ct = conn.getContentType();
        return ct != null ? ct : "application/octet-stream";
    }
}
