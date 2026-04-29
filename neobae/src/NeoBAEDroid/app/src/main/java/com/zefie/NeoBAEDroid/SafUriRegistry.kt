package com.zefie.NeoBAEDroid

import android.net.Uri
import java.util.concurrent.ConcurrentHashMap

object SafUriRegistry {
    private val uriByCachePath = ConcurrentHashMap<String, Uri>()

    fun register(cachePath: String, uri: Uri) {
        uriByCachePath[cachePath] = uri
    }

    fun getUri(cachePath: String): Uri? = uriByCachePath[cachePath]
}
