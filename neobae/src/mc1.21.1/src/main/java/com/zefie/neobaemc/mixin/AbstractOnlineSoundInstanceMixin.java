package com.zefie.neobaemc.mixin;

import com.zefie.neobaemc.NeoBAEMC;
import com.zefie.neobaemc.audio.NeoBAEAudioStream;
import gg.moonflower.etched.api.record.TrackData;
import gg.moonflower.etched.api.sound.AbstractOnlineSoundInstance;
import gg.moonflower.etched.api.sound.source.AudioSource;
import gg.moonflower.etched.api.sound.stream.MonoWrapper;
import gg.moonflower.etched.client.sound.EmptyAudioStream;
import gg.moonflower.etched.client.sound.SoundCache;
import net.minecraft.Util;
import net.minecraft.client.resources.sounds.Sound;
import net.minecraft.client.sounds.AudioStream;
import net.minecraft.client.sounds.SoundBufferLibrary;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.io.BufferedInputStream;
import java.io.InputStream;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

/**
 * Replaces Etched's built-in OGG/WAV/MP3 decoder chain with NeoBAE when the
 * native library is loaded. NeoBAE handles a strict superset of what Etched
 * decodes natively (MP3, OGG Vorbis/Opus, WAV, FLAC, MIDI, RMF, RMI, XMF,
 * MTHC, AIFF, AU, raw PCM), so this gives FLAC and MIDI support "for free"
 * on top of what Etched already does.
 *
 * <p>If NeoBAE is not available, this injection does nothing and Etched's
 * original code path runs.</p>
 *
 * <p>If NeoBAE is loaded but fails to recognise the downloaded data, the
 * stream resolves to {@link EmptyAudioStream#INSTANCE} (the same fallback
 * Etched uses for unrecoverable decode errors). We do NOT fall back to
 * Etched's original chain on a per-track basis — when NeoBAE is loaded it
 * "owns" the decode path. This keeps the mixin simple and predictable; if
 * you need Etched's decoder back, remove this mod.</p>
 */
@Mixin(value = AbstractOnlineSoundInstance.class, remap = false)
public abstract class AbstractOnlineSoundInstanceMixin {

    @Inject(
            method = "getStream(Lnet/minecraft/client/sounds/SoundBufferLibrary;Lnet/minecraft/client/resources/sounds/Sound;Z)Ljava/util/concurrent/CompletableFuture;",
            at = @At("HEAD"),
            cancellable = true,
            remap = false // NeoForge 1.20.5+ runs Mojang-named bytecode; nothing to remap
    )
    private void neobaemc$overrideDecoder(SoundBufferLibrary loader,
                                          Sound sound,
                                          boolean repeatInstantly,
                                          CallbackInfoReturnable<CompletableFuture<AudioStream>> cir) {
        // Only handle Etched's online-sound subtype; pass through anything else
        if (!(sound instanceof AbstractOnlineSoundInstance.OnlineSound onlineSound)) {
            NeoBAEMC.LOGGER.debug("NeoBAE skip: sound is {} not OnlineSound", sound.getClass().getName());
            return;
        }
        // Local sound events (etched://soundevent) — leave to Etched's original code path
        if (TrackData.isLocalSound(onlineSound.getURL())) {
            NeoBAEMC.LOGGER.debug("NeoBAE skip: local sound {}", onlineSound.getURL());
            return;
        }
        // Native engine not loaded — pass through to Etched
        if (!NeoBAEAudioStream.engineAvailable()) {
            NeoBAEMC.LOGGER.warn("NeoBAE skip: engine unavailable, passing {} to Etched's decoder chain",
                    onlineSound.getURL());
            return;
        }
        NeoBAEMC.LOGGER.info("NeoBAE handling {}", onlineSound.getURL());

        cir.setReturnValue(
            SoundCache.getAudioStream(onlineSound.getURL(),
                                      onlineSound.getProgressListener(),
                                      onlineSound.getAudioFileType())
                .thenCompose(AudioSource::openStream)
                .thenApplyAsync(rawStream -> {
                    onlineSound.getProgressListener().progressStartLoading();
                    try (InputStream in = new BufferedInputStream(rawStream)) {
                        NeoBAEAudioStream nba = NeoBAEAudioStream.tryDecode(in);
                        if (nba == null) {
                            NeoBAEMC.LOGGER.warn("NeoBAE did not recognise audio from {}", onlineSound.getURL());
                            return (AudioStream) EmptyAudioStream.INSTANCE;
                        }
                        // Honour the OnlineSound's stereo/mono preference, like Etched does.
                        return sound instanceof gg.moonflower.etched.api.sound.SoundStreamModifier mod
                                ? mod.modifyStream(nba)
                                : new MonoWrapper(nba);
                    } catch (Exception e) {
                        throw new CompletionException(e);
                    }
                }, Util.nonCriticalIoPool())
                .handleAsync((stream, throwable) -> {
                    if (throwable != null) {
                        if (throwable instanceof CompletionException ce && ce.getCause() != null) {
                            throwable = ce.getCause();
                        }
                        NeoBAEMC.LOGGER.error("NeoBAE decode failed for {}", onlineSound.getURL(), throwable);
                        onlineSound.getProgressListener().onFail();
                        return EmptyAudioStream.INSTANCE;
                    }
                    onlineSound.getProgressListener().onSuccess();
                    return stream;
                }, Util.nonCriticalIoPool())
        );
    }
}
