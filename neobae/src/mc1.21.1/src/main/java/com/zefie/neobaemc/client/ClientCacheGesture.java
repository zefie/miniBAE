package com.zefie.neobaemc.client;

import com.zefie.neobaemc.NeoBAEMC;
import com.zefie.neobaemc.net.SoundCacheClearPayload;
import gg.moonflower.etched.api.record.PlayableRecord;
import gg.moonflower.etched.api.record.TrackData;
import net.minecraft.ChatFormatting;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.Component;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.neoforged.api.distmarker.Dist;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.bus.api.SubscribeEvent;
import net.neoforged.fml.common.EventBusSubscriber;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.event.entity.player.PlayerInteractEvent;
import net.neoforged.neoforge.network.PacketDistributor;
import net.neoforged.neoforge.network.registration.NetworkRegistry;

import java.util.List;
import java.util.Objects;

/**
 * Client-side input handler: when the local player sneak+right-clicks
 * while holding an Etched music disc, ask the server to evict the
 * cached transcode for every URL on that disc.
 *
 * <p>Use case: the server transcoded a partial / corrupted download
 * (most often because the disc was ejected mid-process) and we want
 * to force a re-fetch without restarting the server. The disc itself
 * is untouched; only the server's {@code <world>/neobaemc-cache/}
 * entry for the URL is removed.</p>
 */
@EventBusSubscriber(modid = NeoBAEMC.MODID, value = Dist.CLIENT)
public final class ClientCacheGesture {

    private ClientCacheGesture() {}

    @SubscribeEvent
    public static void onRightClick(PlayerInteractEvent.RightClickItem event) {
        // Game bus fires both sides; we only care about client input.
        if (!event.getLevel().isClientSide()) return;

        Player player = event.getEntity();
        if (!player.isShiftKeyDown()) return;

        ItemStack stack = event.getItemStack();
        if (!PlayableRecord.isPlayableRecord(stack)) return;

        // Need a registry lookup to decode the disc's tracks NBT.
        var lookup = player.level().registryAccess();
        List<TrackData> tracks = PlayableRecord.getTracks(lookup, stack);
        if (tracks == null || tracks.isEmpty()) return;

        List<String> urls = tracks.stream()
                .map(TrackData::url)
                .filter(Objects::nonNull)
                .filter(u -> !u.isEmpty())
                .filter(u -> !TrackData.isLocalSound(u)) // local soundevents have no cache
                .distinct()
                .toList();
        if (urls.isEmpty()) return;

        var conn = Minecraft.getInstance().getConnection();
        if (conn == null || !NetworkRegistry.hasChannel(conn, SoundCacheClearPayload.TYPE.id())) {
            player.displayClientMessage(
                    Component.literal("[NeoBAE] Server does not support cache clearing.").withStyle(ChatFormatting.RED),
                    true);
            return;
        }

        try {
            PacketDistributor.sendToServer(new SoundCacheClearPayload(urls));
            player.displayClientMessage(
                    Component.literal("[NeoBAE] Cleared server cache for " + urls.size() + " track(s).")
                            .withStyle(ChatFormatting.AQUA),
                    true);
        } catch (Throwable t) {
            NeoBAEMC.LOGGER.error("NeoBAE: failed to send cache-clear", t);
        }
    }

    /** Optional manual registration hook; the {@link EventBusSubscriber} annotation handles it normally. */
    public static void register(IEventBus modBus) {
        NeoForge.EVENT_BUS.register(ClientCacheGesture.class);
    }
}
