package com.zefie.NeoBAEDroid

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.size
import androidx.compose.material.Icon
import androidx.compose.material.MaterialTheme
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material.icons.filled.ArrowDownward
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

internal fun formatFileSize(bytes: Long): String {
    if (bytes < 0) return "0 B"
    val units = arrayOf("B", "KB", "MB", "GB", "TB")
    var value = bytes.toDouble()
    var unitIndex = 0
    while (value >= 1024.0 && unitIndex < units.lastIndex) {
        value /= 1024.0
        unitIndex++
    }
    return if (unitIndex == 0) {
        "${bytes} ${units[unitIndex]}"
    } else {
        String.format("%.1f %s", value, units[unitIndex])
    }
}

internal fun sortPlaylistItems(items: List<PlaylistItem>, sortMode: SortMode): List<PlaylistItem> {
    if (items.size <= 1) return items

    fun safeSize(item: PlaylistItem): Long {
        return resolvePlaylistItemSize(item)
    }

    return when (sortMode) {
        SortMode.NAME_ASC -> items.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.title })
        SortMode.NAME_DESC -> items.sortedWith(compareByDescending(String.CASE_INSENSITIVE_ORDER) { it.title })
        SortMode.SIZE_ASC -> items.sortedWith(compareBy<PlaylistItem>({ safeSize(it) }, { it.title.lowercase() }))
        SortMode.SIZE_DESC -> items.sortedWith(compareByDescending<PlaylistItem> { safeSize(it) }.thenBy { it.title.lowercase() })
    }
}

@Composable
internal fun SortModeIcon(sortMode: SortMode) {
    val (label, arrow, description) = when (sortMode) {
        SortMode.NAME_ASC -> Triple("Name", Icons.Filled.ArrowUpward, "Sort filename A-Z")
        SortMode.NAME_DESC -> Triple("Name", Icons.Filled.ArrowDownward, "Sort filename Z-A")
        SortMode.SIZE_ASC -> Triple("Size", Icons.Filled.ArrowUpward, "Sort size low to high")
        SortMode.SIZE_DESC -> Triple("Size", Icons.Filled.ArrowDownward, "Sort size high to low")
    }

    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = label,
            fontSize = 9.sp,
            lineHeight = 9.sp,
            color = MaterialTheme.colors.onSurface.copy(alpha = 0.75f),
            maxLines = 1
        )

        Box(modifier = Modifier.size(24.dp)) {
            Icon(
                Icons.AutoMirrored.Filled.Sort,
                contentDescription = description,
                modifier = Modifier.align(Alignment.Center)
            )
            Icon(
                arrow,
                contentDescription = null,
                modifier = Modifier
                    .size(12.dp)
                    .align(Alignment.BottomEnd)
            )
        }
    }
}

internal const val BANK_SIZE_LIMIT_BYTES: Long = 4L * 1024L * 1024L * 1024L

internal fun resolvePlaylistItemSize(item: PlaylistItem): Long {
    if (item.sizeBytes >= 0L) return item.sizeBytes
    return runCatching { item.file.length() }.getOrDefault(-1L)
}
@Composable
internal fun formatTime(ms: Int): String {
    if (ms <= 0) return "0:00"
    val totalSeconds = ms / 1000
    val minutes = totalSeconds / 60
    val seconds = totalSeconds % 60
    return String.format("%d:%02d", minutes, seconds)
}
