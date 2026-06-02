package com.zefie.NeoBAEDroid

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import com.zefie.NeoBAEDroid.database.SQLiteHelper
import com.zefie.NeoBAEDroid.database.FileEntity
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File

data class IndexingProgress(
    val isIndexing: Boolean = false,
    val currentPath: String = "",
    val filesIndexed: Int = 0,
    val foldersScanned: Int = 0,
    val totalSize: Long = 0
)

class FileIndexer(private val context: Context) {
    private val database = SQLiteHelper.getInstance(context)
    private fun getValidExtensions(): Set<String> = HomeFragment.getMusicExtensions(context)

    private fun isSafPath(path: String): Boolean {
        return path.startsWith("content://")
    }
    
    private val _progress = MutableStateFlow(IndexingProgress())
    val progress: StateFlow<IndexingProgress> = _progress
    
    private var indexingJob: Job? = null
    private var currentIndexPath: String = "" // Track which path we're indexing
    
    /**
     * Build/rebuild the entire file index for a given directory
     * This indexes the directory and all its subdirectories
     */
    suspend fun rebuildIndex(rootPath: String) = withContext(Dispatchers.IO) {
        if (_progress.value.isIndexing) {
            return@withContext // Already indexing
        }
        
        indexingJob = coroutineContext[Job]
        currentIndexPath = rootPath // Store the root we're indexing
        
        try {
            _progress.value = IndexingProgress(isIndexing = true)
            
            // Clear existing index for this path
            database.clearAll(rootPath)

            val validExtensions = getValidExtensions()
            if (isSafPath(rootPath)) {
                indexSafTree(rootPath, validExtensions)
            } else {
                // Start indexing from specified root
                val root = File(rootPath)
                if (root.exists() && root.isDirectory) {
                    android.util.Log.i("FileIndexer", "Indexing directory: ${root.absolutePath}")
                    indexDirectory(root, validExtensions)
                }
            }
            
            _progress.value = _progress.value.copy(isIndexing = false)
        } catch (e: CancellationException) {
            _progress.value = IndexingProgress(isIndexing = false)
            throw e
        } catch (e: Exception) {
            _progress.value = IndexingProgress(isIndexing = false)
            throw e
        } finally {
            indexingJob = null
        }
    }
    
    /**
     * Incremental update - only index changed files
     */
    suspend fun incrementalUpdate(rootPath: String) = withContext(Dispatchers.IO) {
        if (_progress.value.isIndexing) {
            return@withContext
        }

        if (isSafPath(rootPath)) {
            rebuildIndex(rootPath)
            return@withContext
        }
        
        try {
            _progress.value = IndexingProgress(isIndexing = true)
            
            val root = File(rootPath)
            if (root.exists() && root.isDirectory) {
                val validExtensions = getValidExtensions()
                indexDirectoryIncremental(root, validExtensions)
            }
            
            _progress.value = _progress.value.copy(isIndexing = false)
        } catch (e: Exception) {
            _progress.value = IndexingProgress(isIndexing = false)
            throw e
        }
    }

    private suspend fun indexSafTree(rootUriString: String, validExtensions: Set<String>) {
        val rootUri = try { Uri.parse(rootUriString) } catch (_: Exception) { null } ?: return
        val authority = rootUri.authority ?: return

        val treeDocId = try { DocumentsContract.getTreeDocumentId(rootUri) } catch (_: Exception) { null } ?: return
        val rootDocId = try {
            if (DocumentsContract.isDocumentUri(context, rootUri)) {
                DocumentsContract.getDocumentId(rootUri)
            } else {
                treeDocId
            }
        } catch (_: Exception) {
            treeDocId
        }

        val treeUri = DocumentsContract.buildTreeDocumentUri(authority, treeDocId)

        val queue = ArrayDeque<String>()
        queue.add(rootDocId)

        val batch = mutableListOf<FileEntity>()
        val batchSize = 100

        var filesIndexed = 0
        var foldersScanned = 0
        var totalSize = 0L

        while (queue.isNotEmpty()) {
            val currentDocId = queue.removeFirst()
            val currentDocUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, currentDocId)
            val currentPath = currentDocUri.toString()

            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, currentDocId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED
            )

            _progress.value = _progress.value.copy(
                currentPath = currentPath,
                filesIndexed = filesIndexed,
                foldersScanned = foldersScanned,
                totalSize = totalSize
            )

            try {
                context.contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                    foldersScanned++

                    val idCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                    val nameCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                    val mimeCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
                    val sizeCol = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_SIZE)
                    val modifiedCol = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_LAST_MODIFIED)

                    while (cursor.moveToNext()) {
                        val childDocId = cursor.getString(idCol) ?: continue
                        val name = cursor.getString(nameCol) ?: continue
                        val mimeType = cursor.getString(mimeCol) ?: ""

                        if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                            queue.add(childDocId)
                            continue
                        }

                        val extension = name.substringAfterLast('.', "").lowercase()
                        if (extension !in validExtensions) continue

                        val childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childDocId)
                        val size = if (sizeCol >= 0 && !cursor.isNull(sizeCol)) cursor.getLong(sizeCol) else 0L
                        val lastModified = if (modifiedCol >= 0 && !cursor.isNull(modifiedCol)) {
                            cursor.getLong(modifiedCol)
                        } else {
                            System.currentTimeMillis()
                        }

                        batch.add(
                            FileEntity(
                                path = childUri.toString(),
                                filename = name.substringBeforeLast('.', name),
                                extension = extension,
                                parent_path = currentPath,
                                size = size,
                                last_modified = lastModified
                            )
                        )

                        filesIndexed++
                        totalSize += size

                        if (batch.size >= batchSize) {
                            database.insertFiles(currentIndexPath, batch.toList())
                            batch.clear()
                        }

                        if (filesIndexed % 500 == 0) {
                            yield()
                        }
                    }
                }
            } catch (_: SecurityException) {
                continue
            } catch (_: Exception) {
                continue
            }
        }

        if (batch.isNotEmpty()) {
            database.insertFiles(currentIndexPath, batch)
        }

        _progress.value = _progress.value.copy(
            filesIndexed = filesIndexed,
            foldersScanned = foldersScanned,
            totalSize = totalSize
        )
    }
    
    /**
     * Iterative directory traversal using queue (avoids recursion stack issues)
     */
    private suspend fun indexDirectory(root: File, validExtensions: Set<String>) {
        val queue = ArrayDeque<File>()
        queue.add(root)
        
        val batch = mutableListOf<FileEntity>()
        val batchSize = 100 // Insert in batches for performance
        
        var filesIndexed = 0
        var foldersScanned = 0
        var totalSize = 0L
        
        while (queue.isNotEmpty()) {
            val current = queue.removeFirst()
            
            try {
                _progress.value = _progress.value.copy(
                    currentPath = current.absolutePath,
                    filesIndexed = filesIndexed,
                    foldersScanned = foldersScanned,
                    totalSize = totalSize
                )
                
                val files = current.listFiles() ?: continue
                foldersScanned++
                
                for (file in files) {
                    when {
                        file.isDirectory && file.canRead() -> {
                            // Skip hidden and system directories for performance
                            if (!file.name.startsWith(".") && 
                                file.name != "Android" && 
                                file.name != "DCIM" && 
                                file.name != "Pictures") {
                                queue.add(file)
                            }
                        }
                        file.isFile && file.extension.lowercase() in validExtensions -> {
                            val entity = FileEntity(
                                path = file.absolutePath,
                                filename = file.nameWithoutExtension,
                                extension = file.extension.lowercase(),
                                parent_path = file.parent ?: "",
                                size = file.length(),
                                last_modified = file.lastModified()
                            )
                            batch.add(entity)
                            filesIndexed++
                            totalSize += file.length()
                            
                            // Insert batch when full
                            if (batch.size >= batchSize) {
                                database.insertFiles(currentIndexPath, batch.toList())
                                batch.clear()
                            }
                        }
                    }
                }
                
                // Yield to prevent blocking
                if (filesIndexed % 500 == 0) {
                    yield()
                }
            } catch (e: SecurityException) {
                // Skip inaccessible directories
                continue
            }
        }
        
        // Insert remaining batch
        if (batch.isNotEmpty()) {
            database.insertFiles(currentIndexPath, batch)
        }
        
        _progress.value = _progress.value.copy(
            filesIndexed = filesIndexed,
            foldersScanned = foldersScanned,
            totalSize = totalSize
        )
    }
    
    /**
     * Incremental indexing - only update changed files
     */
    private suspend fun indexDirectoryIncremental(root: File, validExtensions: Set<String>) {
        val queue = ArrayDeque<File>()
        queue.add(root)
        
        val batch = mutableListOf<FileEntity>()
        val batchSize = 100
        
        var filesIndexed = 0
        var foldersScanned = 0
        
        while (queue.isNotEmpty()) {
            val current = queue.removeFirst()
            
            try {
                _progress.value = _progress.value.copy(
                    currentPath = current.absolutePath,
                    filesIndexed = filesIndexed,
                    foldersScanned = foldersScanned
                )
                
                val files = current.listFiles() ?: continue
                foldersScanned++
                
                for (file in files) {
                    when {
                        file.isDirectory && file.canRead() -> {
                            if (!file.name.startsWith(".") && 
                                file.name != "Android" && 
                                file.name != "DCIM" && 
                                file.name != "Pictures") {
                                queue.add(file)
                            }
                        }
                        file.isFile && file.extension.lowercase() in validExtensions -> {
                            // Check if file needs updating
                            val lastModified = database.getLastModified(file.absolutePath)
                            if (lastModified == null || file.lastModified() > lastModified) {
                                val entity = FileEntity(
                                    path = file.absolutePath,
                                    filename = file.nameWithoutExtension,
                                    extension = file.extension.lowercase(),
                                    parent_path = file.parent ?: "",
                                    size = file.length(),
                                    last_modified = file.lastModified()
                                )
                                batch.add(entity)
                                filesIndexed++
                                
                                if (batch.size >= batchSize) {
                                    database.insertFiles(currentIndexPath, batch.toList())
                                    batch.clear()
                                }
                            }
                        }
                    }
                }
                
                if (filesIndexed % 500 == 0) {
                    yield()
                }
            } catch (e: SecurityException) {
                continue
            }
        }
        
        if (batch.isNotEmpty()) {
            database.insertFiles(currentIndexPath, batch)
        }
        
        _progress.value = _progress.value.copy(
            filesIndexed = filesIndexed,
            foldersScanned = foldersScanned
        )
    }
    
    fun cancelIndexing() {
        indexingJob?.cancel()
        _progress.value = IndexingProgress(isIndexing = false)
    }
    
    suspend fun getIndexedFileCount(): Int {
        return withContext(Dispatchers.IO) {
            // Always return total count across all indexes to be consistent
            database.getTotalFileCount()
        }
    }
    
    suspend fun getIndexedFileCountForPath(path: String?): Int {
        return withContext(Dispatchers.IO) {
            if (path == null) return@withContext 0
            database.getFileCountForPath(path)
        }
    }
}
