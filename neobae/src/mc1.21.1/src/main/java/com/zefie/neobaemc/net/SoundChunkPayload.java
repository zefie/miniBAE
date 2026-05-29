package com.zefie.neobaemc.net;

import com.zefie.neobaemc.NeoBAEMC;
import io.netty.buffer.ByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;

/**
 * Server → Client: one chunk of an Ogg/Vorbis stream. The client accumulates
 * chunks for the matching {@code streamId} (write order = packet order, which
 * NeoForge's reliable channel guarantees) and feeds them to a vanilla
 * {@code OggAudioStream}.
 *
 * <p>{@code totalSize} is the total byte length of the encoded stream, sent
 * with every chunk so the client knows when it's complete without needing
 * a separate "done" packet. {@code data} is the raw bytes for this chunk.</p>
 */
public record SoundChunkPayload(int streamId, int totalSize, byte[] data) implements CustomPacketPayload {

    public static final Type<SoundChunkPayload> TYPE =
            new Type<>(ResourceLocation.fromNamespaceAndPath(NeoBAEMC.MODID, "sound_chunk"));

    public static final StreamCodec<ByteBuf, SoundChunkPayload> CODEC =
            StreamCodec.composite(
                    ByteBufCodecs.VAR_INT, SoundChunkPayload::streamId,
                    ByteBufCodecs.VAR_INT, SoundChunkPayload::totalSize,
                    ByteBufCodecs.byteArray(2 * 1024 * 1024), SoundChunkPayload::data,
                    SoundChunkPayload::new
            );

    @Override
    public Type<? extends CustomPacketPayload> type() { return TYPE; }
}
