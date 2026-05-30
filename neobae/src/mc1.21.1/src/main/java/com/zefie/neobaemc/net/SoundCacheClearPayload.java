package com.zefie.neobaemc.net;

import com.zefie.neobaemc.NeoBAEMC;
import io.netty.buffer.ByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;

import java.util.List;

/**
 * Client → Server: please evict the cached transcode(s) for these URLs
 * (the disc's tracks). Sent when a player sneak+uses a music disc.
 *
 * <p>The server deletes any matching cache file and aborts in-flight
 * downloads, so the next time the disc is played the URL is fetched
 * and transcoded fresh.</p>
 */
public record SoundCacheClearPayload(List<String> urls) implements CustomPacketPayload {

    public static final Type<SoundCacheClearPayload> TYPE =
            new Type<>(ResourceLocation.fromNamespaceAndPath(NeoBAEMC.MODID, "sound_cache_clear"));

    public static final StreamCodec<ByteBuf, SoundCacheClearPayload> CODEC =
            StreamCodec.composite(
                    ByteBufCodecs.STRING_UTF8.apply(ByteBufCodecs.list(8)),
                    SoundCacheClearPayload::urls,
                    SoundCacheClearPayload::new
            );

    @Override
    public Type<? extends CustomPacketPayload> type() { return TYPE; }
}
