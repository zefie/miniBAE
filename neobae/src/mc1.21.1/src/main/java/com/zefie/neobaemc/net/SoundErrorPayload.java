package com.zefie.neobaemc.net;

import com.zefie.neobaemc.NeoBAEMC;
import io.netty.buffer.ByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;

/**
 * Server → Client: the server could not satisfy the {@link SoundRequestPayload}
 * (download failed, decoder unhappy, etc.). The client uses this to fail
 * the pending future so the disc falls back / shows red.
 */
public record SoundErrorPayload(int streamId, String message) implements CustomPacketPayload {

    public static final Type<SoundErrorPayload> TYPE =
            new Type<>(ResourceLocation.fromNamespaceAndPath(NeoBAEMC.MODID, "sound_error"));

    public static final StreamCodec<ByteBuf, SoundErrorPayload> CODEC =
            StreamCodec.composite(
                    ByteBufCodecs.VAR_INT, SoundErrorPayload::streamId,
                    ByteBufCodecs.STRING_UTF8, SoundErrorPayload::message,
                    SoundErrorPayload::new
            );

    @Override
    public Type<? extends CustomPacketPayload> type() { return TYPE; }
}
