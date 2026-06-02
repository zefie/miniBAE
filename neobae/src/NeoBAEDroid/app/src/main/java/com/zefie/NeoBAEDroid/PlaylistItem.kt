package com.zefie.NeoBAEDroid

import android.net.Uri
import java.io.File

data class PlaylistItem(
    val file: File,
    val uri: Uri? = null,
    var title: String = if (file.isDirectory) file.name else file.nameWithoutExtension,
    val path: String = uri?.toString() ?: file.absolutePath,
    val id: Long = path.hashCode().toLong(),
    val durationMs: Int = 0, // Will be populated when available
    var isFolder: Boolean = file.isDirectory,
    val sizeBytes: Long = -1L
)
