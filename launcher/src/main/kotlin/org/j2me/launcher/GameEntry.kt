package org.j2me.launcher

import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.toComposeImageBitmap
import org.jetbrains.skia.Image
import java.io.File
import java.util.jar.Manifest
import java.util.zip.ZipFile

/**
 * A single entry in the game library — one JAR plus the metadata we pulled
 * from its MIDP MANIFEST.MF. Icon is optional (not every MIDlet ships one,
 * and not every icon path resolves inside the JAR).
 */
data class GameEntry(
    val jarPath: File,
    val title: String,
    val vendor: String?,
    val version: String?,
    val midletClass: String,
    val iconPath: String?,
    val icon: ImageBitmap?,
) {
    /** Display name — falls back to the JAR filename if the manifest was lacking. */
    val displayTitle: String
        get() = title.ifBlank { jarPath.nameWithoutExtension }
}

/**
 * Parse a MIDP JAR's manifest and return a [GameEntry], or null if the JAR
 * isn't recognizable as a MIDlet (no MIDlet-1 attribute). Loads the icon too
 * when the manifest references one; missing icons are silently null.
 *
 * MIDP-2.0 MANIFEST.MF format for the game pointer:
 *     MIDlet-1: <display name>, <icon path>, <main class>
 * Path entries are `,`-separated; all three fields are trimmed.
 */
fun parseJarEntry(jar: File): GameEntry? {
    return try {
        ZipFile(jar).use { zip ->
            val manifestEntry = zip.getEntry("META-INF/MANIFEST.MF") ?: return null
            val manifest = zip.getInputStream(manifestEntry).use { Manifest(it) }
            val attrs = manifest.mainAttributes

            val midlet1 = attrs.getValue("MIDlet-1") ?: return null
            val parts = midlet1.split(",", limit = 3).map { it.trim() }
            if (parts.size < 3) return null
            val (title, iconPathRaw, midletClass) = parts

            val iconPath = iconPathRaw.takeIf { it.isNotBlank() }?.trimStart('/')
            val icon = iconPath
                ?.let { loadIcon(zip, it) }

            GameEntry(
                jarPath = jar,
                title = title,
                vendor = attrs.getValue("MIDlet-Vendor"),
                version = attrs.getValue("MIDlet-Version"),
                midletClass = midletClass,
                iconPath = iconPath,
                icon = icon,
            )
        }
    } catch (e: Exception) {
        // Malformed JAR, truncated manifest, unreadable file — ignore and move on
        // rather than crashing the whole scan.
        null
    }
}

/**
 * Pull an icon out of the JAR. MIDlet icon paths are unreliable: some point
 * at /icon.png, some at icon.png, some are in subfolders. Try the exact path
 * first, then the basename as a fallback (useful for buggy manifests).
 */
private fun loadIcon(zip: ZipFile, path: String): ImageBitmap? {
    val candidates = listOfNotNull(
        path,
        path.substringAfterLast('/').takeIf { it != path },
    )
    for (p in candidates) {
        val entry = zip.getEntry(p) ?: continue
        return try {
            val bytes = zip.getInputStream(entry).use { it.readBytes() }
            Image.makeFromEncoded(bytes).toComposeImageBitmap()
        } catch (_: Exception) {
            null
        }
    }
    return null
}

/**
 * A single row in the navigable library view — either a subdirectory or a
 * playable game. The launcher UI treats these uniformly: folders are
 * clickable (descend), games are clickable (launch).
 */
sealed class LibraryItem {
    data class Folder(val dir: File) : LibraryItem() {
        val displayName: String get() = dir.name
    }
    data class Game(val entry: GameEntry) : LibraryItem()
}

/**
 * List [dir]'s immediate children: subfolders first (alpha), then JAR-based
 * games (alpha by title). Non-recursive — deeper folders are explored when
 * the user descends into them. Folders with no contents at all are still
 * listed so the user isn't left wondering why an empty dir doesn't appear.
 */
fun scanDir(dir: File): List<LibraryItem> {
    if (!dir.isDirectory) return emptyList()
    val children = dir.listFiles().orEmpty()
    val folders = children
        .filter { it.isDirectory }
        .sortedBy { it.name.lowercase() }
        .map(LibraryItem::Folder)
    val games = children
        .filter { it.isFile && it.extension.equals("jar", ignoreCase = true) }
        .mapNotNull(::parseJarEntry)
        .sortedBy { it.displayTitle.lowercase() }
        .map(LibraryItem::Game)
    return folders + games
}
