package com.zefie.NeoBAEDroid

import android.content.Context
import android.content.SharedPreferences
import android.util.Xml
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.GetApp
import androidx.compose.material.icons.filled.Publish
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.zefie.NeoBAE.Mixer
import java.io.StringReader
import java.io.StringWriter

private const val PREF_NAME = "NeoBAE_prefs"
private const val KEY_ACTIVE_EQ_PRESET = "custom_eq_preset"
private const val KEY_EQ_ENABLED = "eq_enabled"

val BUILT_IN_EQ_PRESETS = mapOf(
    "Flat" to floatArrayOf(0f, 0f, 0f, 0f, 0f),
    "Bass Boost" to floatArrayOf(8f, 4f, 0f, 0f, 0f),
    "Acoustic" to floatArrayOf(4f, 2f, 0f, 2f, 4f),
    "Rock" to floatArrayOf(6f, 2f, -2f, 2f, 6f),
    "Pop" to floatArrayOf(-2f, 4f, 6f, 4f, -2f),
    "Classical" to floatArrayOf(6f, 4f, 0f, 2f, 4f),
    "Vocal" to floatArrayOf(-4f, 0f, 6f, 4f, -2f)
)

data class CustomEQPreset(
    val name: String,
    val gains: FloatArray // 5 bands
)

fun presetToNeoEQXml(preset: CustomEQPreset): String {
    val serializer = Xml.newSerializer()
    val writer = StringWriter()
    serializer.setOutput(writer)
    serializer.startDocument("UTF-8", true)
    serializer.startTag(null, "neoeq")
    serializer.attribute(null, "version", "1")

    serializer.startTag(null, "name")
    serializer.text(preset.name)
    serializer.endTag(null, "name")

    for (i in 0 until 5) {
        serializer.startTag(null, "band")
        serializer.attribute(null, "index", i.toString())
        serializer.attribute(null, "gain", preset.gains[i].toString())
        serializer.endTag(null, "band")
    }

    serializer.endTag(null, "neoeq")
    serializer.endDocument()
    return writer.toString()
}

fun parseNeoEQXml(xml: String): CustomEQPreset? {
    val parser = Xml.newPullParser()
    parser.setInput(StringReader(xml))

    var name: String? = null
    val gains = FloatArray(5) { 0f }

    var eventType = parser.eventType
    while (eventType != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
        if (eventType == org.xmlpull.v1.XmlPullParser.START_TAG) {
            when (parser.name) {
                "name" -> name = parser.nextText().trim()
                "band" -> {
                    val idx = parser.getAttributeValue(null, "index")?.toIntOrNull() ?: -1
                    if (idx in 0 until 5) {
                        parser.getAttributeValue(null, "gain")?.toFloatOrNull()?.let { gains[idx] = it }
                    }
                }
            }
        }
        eventType = parser.next()
    }

    val finalName = name?.trim().orEmpty()
    if (finalName.isEmpty()) return null

    for (i in 0 until 5) {
        gains[i] = gains[i].coerceIn(-12f, 12f)
    }

    return CustomEQPreset(finalName, gains)
}

private fun prefs(ctx: Context): SharedPreferences =
    ctx.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)

fun getActiveCustomEQPresetName(ctx: Context): String? {
    val name = prefs(ctx).getString(KEY_ACTIVE_EQ_PRESET, null)?.trim()
    return if (name.isNullOrEmpty()) null else name
}

fun setActiveCustomEQPresetName(ctx: Context, name: String?) {
    val e = prefs(ctx).edit()
    if (name.isNullOrBlank()) e.remove(KEY_ACTIVE_EQ_PRESET) else e.putString(KEY_ACTIVE_EQ_PRESET, name)
    e.apply()
}

fun loadCustomEQPresetNames(ctx: Context): List<String> {
    val allKeys = prefs(ctx).all.keys
    var maxIdx = -1
    val re = Regex("^custom_eq_(\\d+)_name$")
    for (k in allKeys) {
        val m = re.find(k) ?: continue
        val idx = m.groupValues[1].toIntOrNull() ?: continue
        if (idx > maxIdx) maxIdx = idx
    }

    if (maxIdx < 0) return emptyList()

    val out = ArrayList<String>()
    val p = prefs(ctx)
    for (idx in 0..maxIdx) {
        val name = p.getString("custom_eq_${idx}_name", null)?.trim()
        if (!name.isNullOrEmpty()) out.add(name)
    }
    return out
}

private fun findEQPresetIndexByName(p: SharedPreferences, name: String): Int? {
    val target = name.trim()
    if (target.isEmpty()) return null

    val allKeys = p.all.keys
    val re = Regex("^custom_eq_(\\d+)_name$")
    for (k in allKeys) {
        val m = re.find(k) ?: continue
        val idx = m.groupValues[1].toIntOrNull() ?: continue
        val v = p.getString(k, null) ?: continue
        if (v.trim() == target) return idx
    }
    return null
}

private fun getMaxEQPresetIndex(p: SharedPreferences): Int {
    var maxIdx = -1
    val re = Regex("^custom_eq_(\\d+)_name$")
    for (k in p.all.keys) {
        val m = re.find(k) ?: continue
        val idx = m.groupValues[1].toIntOrNull() ?: continue
        if (idx > maxIdx) maxIdx = idx
    }
    return maxIdx
}

fun snapshotCustomEQFromEngine(name: String): CustomEQPreset {
    val gains = FloatArray(5) { i -> Mixer.getEQGain(i) }
    return CustomEQPreset(name.trim(), gains)
}

fun saveCustomEQPreset(ctx: Context, preset: CustomEQPreset) {
    val p = prefs(ctx)
    val e = p.edit()
    var idx = findEQPresetIndexByName(p, preset.name)
    if (idx == null) {
        idx = getMaxEQPresetIndex(p) + 1
    }
    val prefix = "custom_eq_${idx}"
    e.putString("${prefix}_name", preset.name)
    for (i in 0 until 5) {
        e.putFloat("${prefix}_band_${i}", preset.gains[i])
    }
    e.apply()
}

fun loadCustomEQPreset(ctx: Context, name: String): CustomEQPreset? {
    val p = prefs(ctx)
    val idx = findEQPresetIndexByName(p, name) ?: return null
    val prefix = "custom_eq_${idx}"
    val actualName = p.getString("${prefix}_name", null) ?: return null
    
    val gains = FloatArray(5)
    for (i in 0 until 5) {
        gains[i] = p.getFloat("${prefix}_band_${i}", 0f)
    }

    return CustomEQPreset(actualName, gains)
}

fun deleteCustomEQPreset(ctx: Context, name: String) {
    val p = prefs(ctx)
    val idx = findEQPresetIndexByName(p, name) ?: return
    val e = p.edit()
    val prefix = "custom_eq_${idx}"
    e.remove("${prefix}_name")
    for (i in 0 until 5) {
        e.remove("${prefix}_band_${i}")
    }
    e.apply()
}

fun applyCustomEQPresetToEngine(preset: CustomEQPreset) {
    if (!Mixer.exists()) return
    for (i in 0 until 5) {
        Mixer.setEQGain(i, preset.gains[i])
    }
}
