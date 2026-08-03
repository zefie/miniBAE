package com.zefie.NeoBAEDroid

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File

internal fun getSafParentPath(uriString: String): String? {
    val normalized = uriString.trim().trimEnd('/')
    val uri = try { Uri.parse(normalized) } catch (_: Exception) { null } ?: return null
    if (uri.scheme != "content") return null

    val segments = uri.pathSegments
    if (segments.size < 2) return null
    if (segments.size == 2 && segments[0] == "tree") return null

    val documentIndex = segments.indexOf("document")
    if (documentIndex < 0 || documentIndex + 1 >= segments.size) return null

    val treeId = Uri.decode(segments[1])
    val documentId = Uri.decode(segments[documentIndex + 1])
    val documentParts = documentId.split('/')
    if (documentParts.size <= 1) {
        return "content://${uri.authority}/tree/${Uri.encode(treeId)}"
    }

    val parentDocumentId = documentParts.dropLast(1).joinToString("/")
    return if (parentDocumentId == treeId) {
        "content://${uri.authority}/tree/${Uri.encode(treeId)}"
    } else {
        "content://${uri.authority}/tree/${Uri.encode(segments[1])}/document/${Uri.encode(parentDocumentId)}"
    }
}

internal fun sanitizeFolderPathForCurrentVariant(path: String?): String {
    val normalized = path?.trim().orEmpty()
    val isSafPath = normalized.startsWith("content://")

    return if (BuildConfig.USE_MANAGE_EXTERNAL_STORAGE) {
        // MANAGE_EXTERNAL_STORAGE builds cannot browse SAF document URIs.
        if (normalized.isEmpty() || isSafPath) "/sdcard" else normalized
    } else {
        // SAF builds should always start from a SAF root when possible.
        if (normalized.isEmpty()) "/" else normalized
    }
}

internal fun getDisplayPath(path: String?): String {
    val raw = path?.trim().orEmpty()
    if (raw.isEmpty()) return "No folder selected"
    if (!raw.startsWith("content://")) return raw

    val uri = try { Uri.parse(raw) } catch (_: Exception) { null } ?: return raw
    val segments = uri.pathSegments
    val documentIndex = segments.indexOf("document")
    if (documentIndex >= 0 && documentIndex + 1 < segments.size) {
        return Uri.decode(segments[documentIndex + 1])
    }
    if (segments.size >= 2 && segments[0] == "tree") {
        return Uri.decode(segments[1])
    }
    return Uri.decode(uri.lastPathSegment ?: raw)
}

internal fun getDisplayFolderName(path: String?): String {
    val display = getDisplayPath(path)
    if (display == "No folder selected") return display
    return display.substringAfterLast('/').substringAfterLast(':').ifBlank { display }
}

internal fun isPlaySafVariant(): Boolean {
    return !BuildConfig.USE_MANAGE_EXTERNAL_STORAGE
}

internal fun getSafTreeRootUriString(uriString: String): String? {
    val normalized = uriString.trim().trimEnd('/')
    val uri = try { Uri.parse(normalized) } catch (_: Exception) { null } ?: return null
    if (uri.scheme != "content") return null

    val segments = uri.pathSegments
    if (segments.size < 2) return null

    val treeIndex = segments.indexOf("tree")
    if (treeIndex < 0 || treeIndex + 1 >= segments.size) return null
    return "content://${uri.authority}/tree/${Uri.encode(Uri.decode(segments[treeIndex + 1]))}"
}

internal fun buildFavoritePlaylistItem(context: Context, path: String): PlaylistItem? {
    val normalized = path.trim().trimEnd('/')
    if (normalized.startsWith("content://")) {
        val uri = try { Uri.parse(normalized) } catch (_: Exception) { null } ?: return null
        val doc = DocumentFile.fromSingleUri(context, uri)
            ?: DocumentFile.fromTreeUri(context, uri)
            ?: return null
        if (doc.isDirectory) return null

        val displayName = doc.name ?: uri.lastPathSegment?.substringAfterLast('/') ?: return null
        val cacheFile = safCacheFileForUri(context, uri, displayName)
        SafUriRegistry.register(cacheFile.absolutePath, uri)
        return PlaylistItem(
            file = cacheFile,
            uri = uri,
            title = displayName,
            path = uri.toString(),
            isFolder = false
        )
    }

    val file = File(normalized)
    return if (file.exists() && !file.isDirectory) PlaylistItem(file) else null
}

internal fun safCacheFileForUri(context: Context, uri: Uri, displayName: String): File {
    val safeName = displayName.replace(Regex("[^A-Za-z0-9._-]"), "_")
    val cacheFolder = File(context.cacheDir, "safcache")
    if (!cacheFolder.exists()) cacheFolder.mkdirs()
    val perUriFolder = File(cacheFolder, uri.hashCode().toString())
    if (!perUriFolder.exists()) perUriFolder.mkdirs()
    return File(perUriFolder, safeName)
}

internal data class PersistedSafFolderEntry(
    val uri: Uri,
    val label: String
)

internal fun getPersistedSafFolderEntries(context: Context): List<PersistedSafFolderEntry> {
    return context.contentResolver.persistedUriPermissions
        .asSequence()
        .filter { it.isReadPermission && it.uri.scheme == "content" }
        .mapNotNull { permission ->
            val uri = permission.uri
            val document = DocumentFile.fromTreeUri(context, uri) ?: return@mapNotNull null
            val label = document.name ?: getDisplayFolderName(uri.toString())
            PersistedSafFolderEntry(uri, label)
        }
        .distinctBy { it.uri.toString() }
        .sortedBy { it.label.lowercase() }
        .toList()
}

internal fun pruneUnavailableFavorites(context: Context, favorites: MutableList<String>) {
    val kept = favorites.filter { path ->
        buildFavoritePlaylistItem(context, path) != null
    }
    if (kept.size == favorites.size) return
    favorites.clear()
    favorites.addAll(kept)
}
