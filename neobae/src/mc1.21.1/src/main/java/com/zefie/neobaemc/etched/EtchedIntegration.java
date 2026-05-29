package com.zefie.neobaemc.etched;

import com.zefie.neobaemc.NeoBAEMC;
import net.neoforged.bus.api.IEventBus;

/**
 * Tiny entry point for the Etched-integration package.
 *
 * <p>The real work of overriding Etched's audio decoder happens in the mixin
 * {@code com.zefie.neobaemc.mixin.AbstractOnlineSoundInstanceMixin}, which is
 * installed by NeoForge before this method ever runs. All this class does is
 * log that the bridge is wired up.</p>
 */
public final class EtchedIntegration {
    private EtchedIntegration() {}

    public static void init(IEventBus modBus) {
        NeoBAEMC.LOGGER.info(
            "NeoBAE → Etched decoder override active. " +
            "Etched will use NeoBAE for MP3/OGG/WAV/FLAC/MIDI/RMF/RMI/XMF/Opus/AIFF/AU/MTHC."
        );
    }
}
