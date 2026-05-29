package com.zefie.neobaemc.net;

import com.zefie.neobaemc.NeoBAEMC;
import com.zefie.neobaemc.client.ClientStreamRegistry;
import com.zefie.neobaemc.server.ServerSoundCache;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.bus.api.SubscribeEvent;
import net.neoforged.fml.common.EventBusSubscriber;
import net.neoforged.neoforge.event.server.ServerStartedEvent;
import net.neoforged.neoforge.event.server.ServerStoppingEvent;
import net.neoforged.neoforge.network.event.RegisterPayloadHandlersEvent;
import net.neoforged.neoforge.network.handling.IPayloadContext;
import net.neoforged.neoforge.network.registration.PayloadRegistrar;

/**
 * Registers our three custom packet payloads with NeoForge and wires up
 * the server-lifecycle hooks for {@link ServerSoundCache}.
 *
 * <p>Channels are declared {@code optional()} so vanilla / pre-NeoBAE
 * clients aren't kicked when they connect to a server with the mod.
 * In that case {@link com.zefie.neobaemc.mixin.AbstractOnlineSoundInstanceMixin}
 * falls back to its local download-and-decode path.</p>
 */
@EventBusSubscriber(modid = NeoBAEMC.MODID)
public final class NetworkBindings {

    private NetworkBindings() {}

    public static void register(IEventBus modBus) {
        modBus.addListener(NetworkBindings::onRegisterPayloads);
    }

    private static void onRegisterPayloads(RegisterPayloadHandlersEvent event) {
        PayloadRegistrar registrar = event.registrar("1").optional();

        registrar.playToServer(
                SoundRequestPayload.TYPE,
                SoundRequestPayload.CODEC,
                NetworkBindings::handleSoundRequest);

        registrar.playToClient(
                SoundChunkPayload.TYPE,
                SoundChunkPayload.CODEC,
                NetworkBindings::handleSoundChunk);

        registrar.playToClient(
                SoundErrorPayload.TYPE,
                SoundErrorPayload.CODEC,
                NetworkBindings::handleSoundError);
    }

    // ---- handlers --------------------------------------------------------

    private static void handleSoundRequest(SoundRequestPayload payload, IPayloadContext ctx) {
        // ctx.player() on serverbound is the requesting ServerPlayer.
        ctx.enqueueWork(() -> ServerSoundCache.handleRequest(
                payload.streamId(),
                payload.url(),
                (net.minecraft.server.level.ServerPlayer) ctx.player()));
    }

    private static void handleSoundChunk(SoundChunkPayload payload, IPayloadContext ctx) {
        // Run on the network thread on purpose — the registry pipe is
        // designed for concurrent writes and we don't want to block the
        // main render thread on large audio buffers.
        ClientStreamRegistry.onChunk(payload.streamId(), payload.totalSize(), payload.data());
    }

    private static void handleSoundError(SoundErrorPayload payload, IPayloadContext ctx) {
        ClientStreamRegistry.onError(payload.streamId(), payload.message());
    }

    // ---- server lifecycle ------------------------------------------------

    @SubscribeEvent
    public static void onServerStarted(ServerStartedEvent event) {
        ServerSoundCache.onServerStarted(event.getServer());
    }

    @SubscribeEvent
    public static void onServerStopping(ServerStoppingEvent event) {
        ServerSoundCache.onServerStopping(event.getServer());
    }
}
