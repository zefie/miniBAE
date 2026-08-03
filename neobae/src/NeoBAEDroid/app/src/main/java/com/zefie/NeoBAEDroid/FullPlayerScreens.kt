package com.zefie.NeoBAEDroid

import android.R
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.*
import androidx.compose.material.icons.filled.*
import androidx.compose.material.pullrefresh.PullRefreshIndicator
import androidx.compose.material.pullrefresh.pullRefresh
import androidx.compose.material.pullrefresh.rememberPullRefreshState
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import androidx.core.view.WindowCompat
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zefie.NeoBAE.Mixer
import com.zefie.NeoBAE.Song
import java.io.File
import java.io.IOException
import kotlin.math.roundToInt
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun FullPlayerScreen(
    viewModel: MusicPlayerViewModel,
    onClose: () -> Unit,
    onPlayPause: () -> Unit,
    onNext: () -> Unit,
    onPrevious: () -> Unit,
    onSeek: (Int) -> Unit,
    onStartDrag: () -> Unit,
    onDrag: (Int) -> Unit,
    onVolumeChange: (Int) -> Unit,
    onToggleFavorite: (String) -> Unit,
    showFavorites: Boolean,
    exportCodec: Int,
    onExportRequest: (String, Int) -> Unit,
    getMidiChannelMuteStatus: () -> BooleanArray?,
    onSetMidiChannelMuted: (Int, Boolean) -> Unit,
    onRepeatModeChange: () -> Unit,
    onNavigateToSettings: () -> Unit
) {
    val currentPositionMs = viewModel.currentPositionMs
    val totalDurationMs = viewModel.totalDurationMs
    val currentItem = viewModel.getCurrentItem()
    val isPlaying = viewModel.isPlaying
    val configuration = LocalConfiguration.current
    val isLandscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
    
    // Update position in real-time
    LaunchedEffect(isPlaying) {
        while (isPlaying) {
            kotlinx.coroutines.delay(250)
            viewModel.currentPositionMs = viewModel.currentPositionMs
        }
    }
    
    if (isLandscape) {
        LandscapePlayerLayout(
            viewModel = viewModel,
            currentPositionMs = currentPositionMs,
            totalDurationMs = totalDurationMs,
            currentItem = currentItem,
            isPlaying = isPlaying,
            onClose = onClose,
            onPlayPause = onPlayPause,
            onNext = onNext,
            onPrevious = onPrevious,
            onSeek = onSeek,
            onStartDrag = onStartDrag,
            onDrag = onDrag,
            onVolumeChange = onVolumeChange,
            onToggleFavorite = onToggleFavorite,
            showFavorites = showFavorites,
            exportCodec = exportCodec,
            onExportRequest = onExportRequest,
            getMidiChannelMuteStatus = getMidiChannelMuteStatus,
            onSetMidiChannelMuted = onSetMidiChannelMuted,
            onRepeatModeChange = onRepeatModeChange,
            onNavigateToSettings = onNavigateToSettings
        )
    } else {
        PortraitPlayerLayout(
            viewModel = viewModel,
            currentPositionMs = currentPositionMs,
            totalDurationMs = totalDurationMs,
            currentItem = currentItem,
            isPlaying = isPlaying,
            onClose = onClose,
            onPlayPause = onPlayPause,
            onNext = onNext,
            onPrevious = onPrevious,
            onSeek = onSeek,
            onStartDrag = onStartDrag,
            onDrag = onDrag,
            onVolumeChange = onVolumeChange,
            onToggleFavorite = onToggleFavorite,
            showFavorites = showFavorites,
            exportCodec = exportCodec,
            onExportRequest = onExportRequest,
            getMidiChannelMuteStatus = getMidiChannelMuteStatus,
            onSetMidiChannelMuted = onSetMidiChannelMuted,
            onRepeatModeChange = onRepeatModeChange,
            onNavigateToSettings = onNavigateToSettings
        )
    }
}

@Composable
internal fun PortraitPlayerLayout(
    viewModel: MusicPlayerViewModel,
    currentPositionMs: Int,
    totalDurationMs: Int,
    currentItem: PlaylistItem?,
    isPlaying: Boolean,
    onClose: () -> Unit,
    onPlayPause: () -> Unit,
    onNext: () -> Unit,
    onPrevious: () -> Unit,
    onSeek: (Int) -> Unit,
    onStartDrag: () -> Unit,
    onDrag: (Int) -> Unit,
    onVolumeChange: (Int) -> Unit,
    onToggleFavorite: (String) -> Unit,
    showFavorites: Boolean,
    exportCodec: Int,
    onExportRequest: (String, Int) -> Unit,
    getMidiChannelMuteStatus: () -> BooleanArray?,
    onSetMidiChannelMuted: (Int, Boolean) -> Unit,
    onRepeatModeChange: () -> Unit,
    onNavigateToSettings: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colors.background)
            .verticalScroll(rememberScrollState())
    ) {
        // Top bar with back button and favorite
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onClose) {
                Icon(
                    Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "Back",
                    tint = MaterialTheme.colors.onBackground
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            
            // EQ button
            IconButton(onClick = onNavigateToSettings) {
                Icon(
                    Icons.Filled.Tune,
                    contentDescription = "Equalizer",
                    tint = MaterialTheme.colors.onBackground
                )
            }
            
            // Export button (disabled when repeat mode is SONG to prevent infinite looping)
            currentItem?.let { item ->
                val isSoundFile = HomeFragment.isSoundExtension(item.file.extension)
                val isExportEnabled = !isSoundFile
                IconButton(
                    onClick = {
                        val extension = when (exportCodec) {
                            2 -> ".ogg"
                            3 -> ".flac"
                            else -> ".wav"
                        }
                        val defaultName = item.file.nameWithoutExtension + extension
                        onExportRequest(defaultName, exportCodec)
                    },
                    enabled = isExportEnabled
                ) {
                    Icon(
                        Icons.Filled.GetApp,
                        contentDescription = "Export",
                        tint = if (isExportEnabled) MaterialTheme.colors.onBackground else Color.Gray
                    )
                }

                MidiChannelMuteButton(
                    enabled = !isSoundFile,
                    getMidiChannelMuteStatus = getMidiChannelMuteStatus,
                    onSetMidiChannelMuted = onSetMidiChannelMuted
                )
            }
            
            // Favorite button
            if (showFavorites) {
                currentItem?.let { item ->
                    val isFavorite = viewModel.isFavorite(item.path)
                    IconButton(onClick = { onToggleFavorite(item.path) }) {
                        Icon(
                            if (isFavorite) Icons.Filled.Favorite else Icons.Filled.FavoriteBorder,
                            contentDescription = if (isFavorite) "Remove from favorites" else "Add to favorites",
                            tint = if (isFavorite) MaterialTheme.colors.primary else MaterialTheme.colors.onBackground
                        )
                    }
                }
            }
        }
        
        // Main content centered
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            // Album art placeholder with swipe gesture
            var offsetX by remember { mutableStateOf(0f) }
            
            Box(
                modifier = Modifier
                    .size(120.dp)
                    .offset(x = (offsetX / 5).dp) // Visual feedback when swiping
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0xFF3700B3))
                    .pointerInput(Unit) {
                        detectHorizontalDragGestures(
                            onDragEnd = {
                                if (offsetX > 100) {
                                    // Swipe right - previous
                                    if (viewModel.hasPrevious()) {
                                        onPrevious()
                                    }
                                } else if (offsetX < -100) {
                                    // Swipe left - next
                                    if (viewModel.hasNext()) {
                                        onNext()
                                    }
                                }
                                offsetX = 0f
                            },
                            onHorizontalDrag = { _, dragAmount ->
                                offsetX = (offsetX + dragAmount).coerceIn(-200f, 200f)
                            }
                        )
                    },
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    Icons.Filled.MusicNote,
                    contentDescription = null,
                    modifier = Modifier.size(120.dp),
                    tint = Color.White.copy(alpha = 0.5f)
                )
            }
            
            Spacer(modifier = Modifier.height(32.dp))
            
            // Song title
            Text(
                text = currentItem?.title ?: "No song playing",
                style = MaterialTheme.typography.h5,
                color = MaterialTheme.colors.onBackground,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center
            )
            
            Spacer(modifier = Modifier.height(8.dp))
            
            // Lyrics area
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
                contentAlignment = Alignment.Center
            ) {
                if (viewModel.currentLyric.isNotEmpty()) {
                    Text(
                        text = viewModel.currentLyric,
                        style = MaterialTheme.typography.body1.copy(fontWeight = FontWeight.Bold),
                        color = MaterialTheme.colors.primary,
                        textAlign = TextAlign.Center,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
            
            Spacer(modifier = Modifier.height(8.dp))
            
            // Seek bar
            Column(modifier = Modifier.fillMaxWidth()) {
                var isDragging by remember { mutableStateOf(false) }
                var dragPosition by remember { mutableStateOf(0f) }
                
                Slider(
                    value = if (isDragging) dragPosition else currentPositionMs.toFloat(),
                    onValueChange = {
                        if (!isDragging) {
                            isDragging = true
                            onStartDrag()
                        }
                        dragPosition = it
                        onDrag(it.toInt())
                    },
                    onValueChangeFinished = {
                        isDragging = false
                        onSeek(dragPosition.toInt())
                    },
                    valueRange = 0f..totalDurationMs.toFloat().coerceAtLeast(1f),
                    colors = SliderDefaults.colors(
                        thumbColor = MaterialTheme.colors.primary,
                        activeTrackColor = MaterialTheme.colors.primary,
                        inactiveTrackColor = Color.Gray.copy(alpha = 0.3f)
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
                
                // Time labels
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = formatTime(currentPositionMs),
                        style = MaterialTheme.typography.caption,
                        color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                        modifier = Modifier.clickable { onSeek(0) }
                    )
                    Text(
                        text = formatTime(totalDurationMs),
                        style = MaterialTheme.typography.caption,
                        color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f)
                    )
                }
            }
            
            Spacer(modifier = Modifier.height(24.dp))
            
            // Volume control
            Column(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    val mutedByIcon = viewModel.volumeMutedByIcon
                    IconButton(
                        onClick = {
                            val nextVolume = viewModel.toggleMuteViaIcon()
                            onVolumeChange(nextVolume)
                        }
                    ) {
                        Icon(
                            if (mutedByIcon) Icons.AutoMirrored.Filled.VolumeOff else Icons.AutoMirrored.Filled.VolumeUp,
                            contentDescription = if (mutedByIcon) "Unmute" else "Mute",
                            tint = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    Spacer(modifier = Modifier.width(8.dp))
                    Slider(
                        value = viewModel.volumePercent.toFloat(),
                        onValueChange = {
                            val v = it.toInt()
                            viewModel.onUserDraggedVolume(v)
                            onVolumeChange(v)
                        },
                        valueRange = 0f..100f,
                        colors = SliderDefaults.colors(
                            thumbColor = MaterialTheme.colors.primary,
                            activeTrackColor = MaterialTheme.colors.primary,
                            inactiveTrackColor = Color.Gray.copy(alpha = 0.3f)
                        ),
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "${viewModel.volumePercent}%",
                        style = MaterialTheme.typography.caption,
                        color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                        modifier = Modifier.width(40.dp)
                    )
                }
            }
            
            Spacer(modifier = Modifier.height(24.dp))
            
            // Playback controls
            val hasPrevious = viewModel.hasPrevious()
            val hasNext = viewModel.hasNext()
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(
                    onClick = onPrevious,
                    enabled = hasPrevious,
                    modifier = Modifier.size(56.dp)
                ) {
                    Icon(
                        Icons.Filled.SkipPrevious,
                        contentDescription = "Previous",
                        tint = if (hasPrevious) MaterialTheme.colors.onBackground else MaterialTheme.colors.onBackground.copy(alpha = 0.3f),
                        modifier = Modifier.size(48.dp)
                    )
                }
                
                // Play/Pause button
                FloatingActionButton(
                    onClick = onPlayPause,
                    backgroundColor = MaterialTheme.colors.primary,
                    modifier = Modifier.size(64.dp)
                ) {
                    Icon(
                        if (isPlaying) Icons.Filled.Pause else Icons.Filled.PlayArrow,
                        contentDescription = if (isPlaying) "Pause" else "Play",
                        modifier = Modifier.size(32.dp),
                        tint = Color.White
                    )
                }
                
                IconButton(
                    onClick = onNext,
                    enabled = hasNext,
                    modifier = Modifier.size(56.dp)
                ) {
                    Icon(
                        Icons.Filled.SkipNext,
                        contentDescription = "Next",
                        tint = if (hasNext) MaterialTheme.colors.onBackground else MaterialTheme.colors.onBackground.copy(alpha = 0.3f),
                        modifier = Modifier.size(48.dp)
                    )
                }
            }
            
            Spacer(modifier = Modifier.height(16.dp))
            
            // Repeat and Shuffle buttons
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(
                    onClick = {
                        viewModel.repeatMode = when (viewModel.repeatMode) {
                            RepeatMode.NONE -> RepeatMode.SONG
                            RepeatMode.SONG -> RepeatMode.PLAYLIST
                            RepeatMode.PLAYLIST -> RepeatMode.NONE
                        }
                        // Update loop count for currently playing song
                        onRepeatModeChange()
                    }
                ) {
                    Icon(
                        when (viewModel.repeatMode) {
                            RepeatMode.NONE -> Icons.Filled.Repeat
                            RepeatMode.SONG -> Icons.Filled.RepeatOne
                            RepeatMode.PLAYLIST -> Icons.Filled.Repeat
                        },
                        contentDescription = "Repeat: ${viewModel.repeatMode.name}",
                        tint = if (viewModel.repeatMode == RepeatMode.NONE) {
                            Color.Gray
                        } else {
                            MaterialTheme.colors.primary
                        },
                        modifier = Modifier.size(32.dp)
                    )
                }
                
                Spacer(modifier = Modifier.width(24.dp))
                
                IconButton(
                    onClick = {
                        viewModel.toggleShuffle()
                    }
                ) {
                    Icon(
                        Icons.Filled.Shuffle,
                        contentDescription = "Shuffle: ${if (viewModel.isShuffled) "On" else "Off"}",
                        tint = if (viewModel.isShuffled) {
                            MaterialTheme.colors.primary
                        } else {
                            Color.Gray
                        },
                        modifier = Modifier.size(32.dp)
                    )
                }
            }
            
            Spacer(modifier = Modifier.height(24.dp))
        }
    }
}

@Composable
internal fun MidiChannelMuteButton(
    enabled: Boolean,
    getMidiChannelMuteStatus: () -> BooleanArray?,
    onSetMidiChannelMuted: (Int, Boolean) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val channelEnabled = remember { mutableStateListOf<Boolean>().apply { repeat(16) { add(true) } } }
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    val columns = if (configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE) 6 else 3

    LaunchedEffect(expanded) {
        if (expanded) {
            val status = getMidiChannelMuteStatus()
            if (status != null && status.size >= 16) {
                for (i in 0 until 16) {
                    channelEnabled[i] = !status[i]
                }
            } else {
                for (i in 0 until 16) {
                    channelEnabled[i] = true
                }
            }
        }
    }

    Box {
        IconButton(
            onClick = { expanded = !expanded },
            enabled = enabled
        ) {
            Icon(
                Icons.Filled.GraphicEq,
                contentDescription = "MIDI Channels",
                tint = when {
                    !enabled -> Color.Gray
                    expanded -> MaterialTheme.colors.primary
                    else -> MaterialTheme.colors.onBackground
                }
            )
        }

        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            Column(
                modifier = Modifier
                    .padding(12.dp)
                    .widthIn(min = if (columns == 6) 360.dp else 220.dp)
            ) {
                for (rowStart in 0 until 16 step columns) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        for (col in 0 until columns) {
                            val channel = rowStart + col
                            if (channel < 16) {
                                val checked = channelEnabled[channel]
                                Row(
                                    modifier = Modifier
                                        .weight(1f)
                                        .padding(end = if (col < columns - 1) 8.dp else 0.dp),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Checkbox(
                                        checked = checked,
                                        onCheckedChange = { nextChecked ->
                                            channelEnabled[channel] = nextChecked
                                            onSetMidiChannelMuted(channel, !nextChecked)
                                        }
                                    )
                                    Text(
                                        text = "Ch ${channel + 1}",
                                        color = MaterialTheme.colors.onSurface
                                    )
                                }
                            } else {
                                Spacer(modifier = Modifier.weight(1f))
                            }
                        }
                    }
                }

                Spacer(modifier = Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Button(
                        onClick = {
                            for (channel in 0 until 16) {
                                channelEnabled[channel] = true
                                onSetMidiChannelMuted(channel, false)
                            }
                        },
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("All")
                    }
                    Spacer(modifier = Modifier.width(8.dp))
                    Button(
                        onClick = {
                            for (channel in 0 until 16) {
                                val nextChecked = !channelEnabled[channel]
                                channelEnabled[channel] = nextChecked
                                onSetMidiChannelMuted(channel, !nextChecked)
                            }
                        },
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Invert")
                    }
                }
            }
        }
    }
}

@Composable
internal fun LandscapePlayerLayout(
    viewModel: MusicPlayerViewModel,
    currentPositionMs: Int,
    totalDurationMs: Int,
    currentItem: PlaylistItem?,
    isPlaying: Boolean,
    onClose: () -> Unit,
    onPlayPause: () -> Unit,
    onNext: () -> Unit,
    onPrevious: () -> Unit,
    onSeek: (Int) -> Unit,
    onStartDrag: () -> Unit,
    onDrag: (Int) -> Unit,
    onVolumeChange: (Int) -> Unit,
    onToggleFavorite: (String) -> Unit,
    showFavorites: Boolean,
    exportCodec: Int,
    onExportRequest: (String, Int) -> Unit,
    getMidiChannelMuteStatus: () -> BooleanArray?,
    onSetMidiChannelMuted: (Int, Boolean) -> Unit,
    onRepeatModeChange: () -> Unit,
    onNavigateToSettings: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colors.background)
    ) {
        // Top bar with back button and actions
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onClose) {
                Icon(
                    Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "Back",
                    tint = MaterialTheme.colors.onBackground
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            
            // EQ button
            IconButton(onClick = onNavigateToSettings) {
                Icon(
                    Icons.Filled.Tune,
                    contentDescription = "Equalizer",
                    tint = MaterialTheme.colors.onBackground
                )
            }
            
            // Export button
            currentItem?.let { item ->
                val isSoundFile = HomeFragment.isSoundExtension(item.file.extension)
                val isExportEnabled = !isSoundFile
                IconButton(
                    onClick = {
                        val extension = when (exportCodec) {
                            2 -> ".ogg"
                            3 -> ".flac"
                            else -> ".wav"
                        }
                        val defaultName = item.file.nameWithoutExtension + extension
                        onExportRequest(defaultName, exportCodec)
                    },
                    enabled = isExportEnabled
                ) {
                    Icon(
                        Icons.Filled.GetApp,
                        contentDescription = "Export",
                        tint = if (isExportEnabled) MaterialTheme.colors.onBackground else Color.Gray
                    )
                }

                MidiChannelMuteButton(
                    enabled = !isSoundFile,
                    getMidiChannelMuteStatus = getMidiChannelMuteStatus,
                    onSetMidiChannelMuted = onSetMidiChannelMuted
                )
            }
            
            // Favorite button
            if (showFavorites) {
                currentItem?.let { item ->
                    val isFavorite = viewModel.isFavorite(item.path)
                    IconButton(onClick = { onToggleFavorite(item.path) }) {
                        Icon(
                            if (isFavorite) Icons.Filled.Favorite else Icons.Filled.FavoriteBorder,
                            contentDescription = if (isFavorite) "Remove from favorites" else "Add to favorites",
                            tint = if (isFavorite) MaterialTheme.colors.primary else MaterialTheme.colors.onBackground
                        )
                    }
                }
            }
        }
        
        // Main content in horizontal layout
        Row(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 24.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Left side: Album art and title
            Column(
                modifier = Modifier
                    .width(200.dp)
                    .padding(end = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                // Album art with swipe gesture
                var offsetX by remember { mutableStateOf(0f) }
                
                Box(
                    modifier = Modifier
                        .size(160.dp)
                        .offset(x = (offsetX / 5).dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(Color(0xFF3700B3))
                        .pointerInput(Unit) {
                            detectHorizontalDragGestures(
                                onDragEnd = {
                                    if (offsetX > 100) {
                                        if (viewModel.hasPrevious()) {
                                            onPrevious()
                                        }
                                    } else if (offsetX < -100) {
                                        if (viewModel.hasNext()) {
                                            onNext()
                                        }
                                    }
                                    offsetX = 0f
                                },
                                onHorizontalDrag = { _, dragAmount ->
                                    offsetX = (offsetX + dragAmount).coerceIn(-200f, 200f)
                                }
                            )
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        Icons.Filled.MusicNote,
                        contentDescription = null,
                        modifier = Modifier.size(120.dp),
                        tint = Color.White.copy(alpha = 0.5f)
                    )
                }
                
                Spacer(modifier = Modifier.height(16.dp))
                
                // Song title
                Text(
                    text = currentItem?.title ?: "No song playing",
                    style = MaterialTheme.typography.subtitle1,
                    color = MaterialTheme.colors.onBackground,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.Center
                )
            }
            
            // Right side: Controls and seek bar
            Column(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight(),
                verticalArrangement = Arrangement.Center
            ) {
                // Lyrics area (compact)
                if (viewModel.currentLyric.isNotEmpty()) {
                    Text(
                        text = viewModel.currentLyric,
                        style = MaterialTheme.typography.body2.copy(fontWeight = FontWeight.Bold),
                        color = MaterialTheme.colors.primary,
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                }
                
                // Playback controls
                val hasPrevious = viewModel.hasPrevious()
                val hasNext = viewModel.hasNext()
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(
                        onClick = onPrevious,
                        enabled = hasPrevious,
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            Icons.Filled.SkipPrevious,
                            contentDescription = "Previous",
                            tint = if (hasPrevious) MaterialTheme.colors.onBackground else MaterialTheme.colors.onBackground.copy(alpha = 0.3f),
                            modifier = Modifier.size(40.dp)
                        )
                    }
                    
                    Spacer(modifier = Modifier.width(16.dp))
                    
                    // Play/Pause button
                    FloatingActionButton(
                        onClick = onPlayPause,
                        backgroundColor = MaterialTheme.colors.primary,
                        modifier = Modifier.size(56.dp)
                    ) {
                        Icon(
                            if (isPlaying) Icons.Filled.Pause else Icons.Filled.PlayArrow,
                            contentDescription = if (isPlaying) "Pause" else "Play",
                            modifier = Modifier.size(28.dp),
                            tint = Color.White
                        )
                    }
                    
                    Spacer(modifier = Modifier.width(16.dp))
                    
                    IconButton(
                        onClick = onNext,
                        enabled = hasNext,
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            Icons.Filled.SkipNext,
                            contentDescription = "Next",
                            tint = if (hasNext) MaterialTheme.colors.onBackground else MaterialTheme.colors.onBackground.copy(alpha = 0.3f),
                            modifier = Modifier.size(40.dp)
                        )
                    }
                }
                
                Spacer(modifier = Modifier.height(12.dp))
                
                // Repeat and Shuffle buttons
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(
                        onClick = {
                            viewModel.repeatMode = when (viewModel.repeatMode) {
                                RepeatMode.NONE -> RepeatMode.SONG
                                RepeatMode.SONG -> RepeatMode.PLAYLIST
                                RepeatMode.PLAYLIST -> RepeatMode.NONE
                            }
                            onRepeatModeChange()
                        }
                    ) {
                        Icon(
                            when (viewModel.repeatMode) {
                                RepeatMode.NONE -> Icons.Filled.Repeat
                                RepeatMode.SONG -> Icons.Filled.RepeatOne
                                RepeatMode.PLAYLIST -> Icons.Filled.Repeat
                            },
                            contentDescription = "Repeat: ${viewModel.repeatMode.name}",
                            tint = if (viewModel.repeatMode == RepeatMode.NONE) {
                                Color.Gray
                            } else {
                                MaterialTheme.colors.primary
                            },
                            modifier = Modifier.size(28.dp)
                        )
                    }
                    
                    Spacer(modifier = Modifier.width(16.dp))
                    
                    IconButton(
                        onClick = {
                            viewModel.toggleShuffle()
                        }
                    ) {
                        Icon(
                            Icons.Filled.Shuffle,
                            contentDescription = "Shuffle: ${if (viewModel.isShuffled) "On" else "Off"}",
                            tint = if (viewModel.isShuffled) {
                                MaterialTheme.colors.primary
                            } else {
                                Color.Gray
                            },
                            modifier = Modifier.size(28.dp)
                        )
                    }
                }
                
                Spacer(modifier = Modifier.height(12.dp))
                
                // Seek bar
                Column(modifier = Modifier.fillMaxWidth()) {
                    var isDragging by remember { mutableStateOf(false) }
                    var dragPosition by remember { mutableStateOf(0f) }
                    
                    Slider(
                        value = if (isDragging) dragPosition else currentPositionMs.toFloat(),
                        onValueChange = {
                            if (!isDragging) {
                                isDragging = true
                                onStartDrag()
                            }
                            dragPosition = it
                            onDrag(it.toInt())
                        },
                        onValueChangeFinished = {
                            isDragging = false
                            onSeek(dragPosition.toInt())
                        },
                        valueRange = 0f..totalDurationMs.toFloat().coerceAtLeast(1f),
                        colors = SliderDefaults.colors(
                            thumbColor = MaterialTheme.colors.primary,
                            activeTrackColor = MaterialTheme.colors.primary,
                            inactiveTrackColor = Color.Gray.copy(alpha = 0.3f)
                        ),
                        modifier = Modifier.fillMaxWidth()
                    )
                    
                    // Time labels
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(
                            text = formatTime(currentPositionMs),
                            style = MaterialTheme.typography.caption,
                            color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                            modifier = Modifier.clickable { onSeek(0) }
                        )
                        Text(
                            text = formatTime(totalDurationMs),
                            style = MaterialTheme.typography.caption,
                            color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f)
                        )
                    }
                }
                
                Spacer(modifier = Modifier.height(12.dp))
                
                // Volume control
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    val mutedByIcon = viewModel.volumeMutedByIcon
                    IconButton(
                        onClick = {
                            val nextVolume = viewModel.toggleMuteViaIcon()
                            onVolumeChange(nextVolume)
                        }
                    ) {
                        Icon(
                            if (mutedByIcon) Icons.AutoMirrored.Filled.VolumeOff else Icons.AutoMirrored.Filled.VolumeUp,
                            contentDescription = if (mutedByIcon) "Unmute" else "Mute",
                            tint = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    Spacer(modifier = Modifier.width(8.dp))
                    Slider(
                        value = viewModel.volumePercent.toFloat(),
                        onValueChange = {
                            val v = it.toInt()
                            viewModel.onUserDraggedVolume(v)
                            onVolumeChange(v)
                        },
                        valueRange = 0f..100f,
                        colors = SliderDefaults.colors(
                            thumbColor = MaterialTheme.colors.primary,
                            activeTrackColor = MaterialTheme.colors.primary,
                            inactiveTrackColor = Color.Gray.copy(alpha = 0.3f)
                        ),
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "${viewModel.volumePercent}%",
                        style = MaterialTheme.typography.caption,
                        color = MaterialTheme.colors.onBackground.copy(alpha = 0.7f),
                        modifier = Modifier.width(40.dp)
                    )
                }
            }
        }
    }
}

