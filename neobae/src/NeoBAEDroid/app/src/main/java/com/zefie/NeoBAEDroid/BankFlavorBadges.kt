package com.zefie.NeoBAEDroid

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.MaterialTheme
import androidx.compose.material.Surface
import androidx.compose.material.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

internal fun buildGitHubUrlForBaeVersion(versionString: String): String? {
    val raw = versionString.trim()
    if (raw.isEmpty()) return null

    if (raw.startsWith("git-")) {
        val sha = raw.removePrefix("git-").takeWhile { it != '-' }
        return if (sha.isNotBlank()) {
            "https://github.com/zefie/NeoBAE/commit/$sha"
        } else {
            null
        }
    }

    if (raw.startsWith("built", ignoreCase = true)) return null

    val cleaned = if (raw.contains("-dirty")) raw.substringBefore("-dirty") else raw
    if (cleaned.contains("dirty", ignoreCase = true)) return null

    return "https://github.com/zefie/NeoBAE/tree/$cleaned"
}

internal data class BankFlavorBadge(
    val label: String,
    val fill: Color,
    val text: Color,
)

/** Match zefidi STATUS & BANK flavor chips (miniBAE / NeoBAE / FluidBAE / DLS / XMF / RMI). */
internal fun resolveBankFlavorBadges(
    bankPath: String,
    hasEggsBank: Boolean,
    hasMobileBAEBank: Boolean,
    dlsBankLevel: Int,
    dlsCompatibilityMode: Boolean,
    hasXmfOverlay: Boolean,
    hasRmiEmbedded: Boolean,
    rmiUsesSf2: Boolean,
    dark: Boolean,
): List<BankFlavorBadge> {
    val path = bankPath.ifBlank { "__builtin__" }
    val lower = path.lowercase()
    val badges = mutableListOf<BankFlavorBadge>()
    fun rgba(r: Int, g: Int, b: Int, a: Int) = Color(r, g, b, a)
    fun neoDlsBadge(): BankFlavorBadge {
        val label = if (dlsBankLevel == 2) "NeoBAE DLS 2" else "NeoBAE DLS 1"
        return if (dark) {
            BankFlavorBadge(label, rgba(85, 50, 25, 220), rgba(255, 185, 110, 255))
        } else {
            BankFlavorBadge(label, rgba(255, 205, 145, 230), rgba(100, 50, 15, 255))
        }
    }
    fun mobileBadge(): BankFlavorBadge =
        if (dark) {
            BankFlavorBadge("mobileBAE", rgba(30, 70, 95, 220), rgba(160, 220, 255, 255))
        } else {
            BankFlavorBadge("mobileBAE", rgba(190, 225, 245, 230), rgba(20, 70, 110, 255))
        }
    fun appendDlsHostBadges(): Boolean {
        var addedMobile = false
        when {
            hasEggsBank -> {
                badges += if (dark) {
                    BankFlavorBadge("microQ", rgba(40, 40, 40, 220), rgba(235, 235, 235, 255))
                } else {
                    BankFlavorBadge("microQ", rgba(230, 230, 230, 230), rgba(25, 25, 25, 255))
                }
            }
            hasMobileBAEBank -> {
                badges += mobileBadge()
                addedMobile = true
            }
            else -> {
                if (!dlsCompatibilityMode) {
                    badges += mobileBadge()
                    addedMobile = true
                }
                badges += neoDlsBadge()
            }
        }
        return addedMobile
    }

    var hasMobileChip = false
    if (hasRmiEmbedded) {
        /* RMI replaces the host bank — badge from the embed only. */
        when {
            hasEggsBank || hasMobileBAEBank || dlsBankLevel > 0 -> {
                appendDlsHostBadges()
            }
            rmiUsesSf2 -> {
                badges += if (dark) {
                    BankFlavorBadge("FluidBAE", rgba(20, 70, 65, 220), rgba(130, 235, 215, 255))
                } else {
                    BankFlavorBadge("FluidBAE", rgba(175, 235, 225, 230), rgba(15, 90, 80, 255))
                }
            }
        }
    } else {
        when {
            path == "__builtin__" || lower.endsWith(".hsb") -> {
                badges += if (dark) {
                    BankFlavorBadge("miniBAE", rgba(20, 35, 85, 220), rgba(150, 180, 255, 255))
                } else {
                    BankFlavorBadge("miniBAE", rgba(30, 50, 120, 230), rgba(220, 230, 255, 255))
                }
            }
            lower.endsWith(".zsb") -> {
                badges += if (dark) {
                    BankFlavorBadge("NeoBAE", rgba(90, 70, 30, 220), rgba(255, 220, 120, 255))
                } else {
                    BankFlavorBadge("NeoBAE", rgba(255, 230, 160, 230), rgba(120, 80, 20, 255))
                }
            }
            lower.endsWith(".sf2") || lower.endsWith(".sf3") || lower.endsWith(".sfo") -> {
                badges += if (dark) {
                    BankFlavorBadge("FluidBAE", rgba(20, 70, 65, 220), rgba(130, 235, 215, 255))
                } else {
                    BankFlavorBadge("FluidBAE", rgba(175, 235, 225, 230), rgba(15, 90, 80, 255))
                }
            }
            lower.endsWith(".dls") || hasEggsBank || hasMobileBAEBank || dlsBankLevel > 0 -> {
                hasMobileChip = appendDlsHostBadges()
            }
        }
        /* XMF overlay: add mobileBAE alongside host (no duplicate). */
        if (hasXmfOverlay && !hasMobileChip && badges.none { it.label == "mobileBAE" }) {
            badges += mobileBadge()
        }
    }
    return badges
}

@Composable
internal fun BankFlavorBadgesRow(
    bankPath: String,
    hasEggsBank: Boolean,
    hasMobileBAEBank: Boolean,
    dlsBankLevel: Int,
    dlsCompatibilityMode: Boolean,
    hasXmfOverlay: Boolean,
    hasRmiEmbedded: Boolean,
    rmiUsesSf2: Boolean,
) {
    val dark = isSystemInDarkTheme()
    val badges = remember(
        bankPath, hasEggsBank, hasMobileBAEBank, dlsBankLevel, dlsCompatibilityMode,
        hasXmfOverlay, hasRmiEmbedded, rmiUsesSf2, dark,
    ) {
        resolveBankFlavorBadges(
            bankPath,
            hasEggsBank,
            hasMobileBAEBank,
            dlsBankLevel,
            dlsCompatibilityMode,
            hasXmfOverlay,
            hasRmiEmbedded,
            rmiUsesSf2,
            dark,
        )
    }
    for (badge in badges) {
        Spacer(modifier = Modifier.width(8.dp))
        Surface(
            shape = RoundedCornerShape(4.dp),
            color = badge.fill,
            contentColor = badge.text,
            border = BorderStroke(1.dp, badge.text),
        ) {
            Text(
                text = badge.label,
                style = MaterialTheme.typography.caption,
                fontWeight = FontWeight.Bold,
                color = badge.text,
                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                maxLines = 1,
            )
        }
    }
}
