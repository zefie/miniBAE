package com.zefie.neobaemc;

import com.mojang.logging.LogUtils;
import com.zefie.neobaemc.audio.NativeLoader;
import com.zefie.neobaemc.etched.EtchedIntegration;
import com.zefie.neobaemc.net.NetworkBindings;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.common.Mod;
import org.slf4j.Logger;

/**
 * Mod entry point.
 *
 * <p>This mod hard-depends on Etched. It overrides Etched's built-in
 * OGG/WAV/MP3 decoder cascade with the NeoBAE engine via a mixin (see
 * {@code com.zefie.neobaemc.mixin.AbstractOnlineSoundInstanceMixin}). The
 * mixin runs entirely on the client; this constructor only logs status
 * and pre-loads the native library so failures surface on startup rather
 * than the first time a disc is played.</p>
 */
@Mod(NeoBAEMC.MODID)
public class NeoBAEMC {
    public static final String MODID = "neobaemc";
    public static final Logger LOGGER = LogUtils.getLogger();

    public NeoBAEMC(IEventBus modBus) {
        try {
            if (NativeLoader.ensureLoaded()) {
                LOGGER.info("NeoBAE native library loaded.");
            } else {
                LOGGER.error("NeoBAE native library NOT loaded; Etched will use its built-in decoders.");
            }
        } catch (Throwable t) {
            LOGGER.error("Unexpected error initialising NeoBAE.", t);
        }

        EtchedIntegration.init(modBus);
        NetworkBindings.register(modBus);
    }
}
