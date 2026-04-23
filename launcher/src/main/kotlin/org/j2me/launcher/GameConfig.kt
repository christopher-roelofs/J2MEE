package org.j2me.launcher

import java.io.File

/**
 * Per-game preferences stored at ``~/.j2me/<jar-stem>/config.json``. Same
 * format the native runtime reads — see runtime/src/util/game_config.cpp.
 *
 * We use a hand-rolled tiny JSON parser/writer rather than pulling in a
 * full library because the schema is three keys and the file is short.
 * If the schema grows, swap to kotlinx.serialization.
 */
data class GameConfig(
    var resolution: String = "auto",
    var keypadLayout: String = "minimal",
    var keypadVisible: Boolean = true,
) {
    companion object {
        fun fileFor(jarStem: String): File {
            val home = System.getProperty("user.home")
            return File(home, ".j2me/$jarStem/config.json")
        }

        /** Returns a config populated from disk, or defaults if missing/malformed. */
        fun load(jarStem: String): GameConfig {
            val cfg = GameConfig()
            val f = fileFor(jarStem)
            if (!f.isFile) return cfg
            val text = runCatching { f.readText() }.getOrNull() ?: return cfg
            // Three string/bool fields — extract by regex. Tolerant of
            // formatting quirks (whitespace, key order, missing keys).
            Regex(""""resolution"\s*:\s*"([^"]*)"""").find(text)?.let {
                cfg.resolution = it.groupValues[1]
            }
            Regex(""""keypad_layout"\s*:\s*"([^"]*)"""").find(text)?.let {
                cfg.keypadLayout = it.groupValues[1]
            }
            Regex(""""keypad_visible"\s*:\s*(true|false)""").find(text)?.let {
                cfg.keypadVisible = it.groupValues[1] == "true"
            }
            return cfg
        }
    }

    fun save(jarStem: String): Boolean {
        val f = fileFor(jarStem)
        f.parentFile?.mkdirs()
        val json = buildString {
            append("{\n")
            append("  \"resolution\": ").append(jsonStr(resolution)).append(",\n")
            append("  \"keypad_layout\": ").append(jsonStr(keypadLayout)).append(",\n")
            append("  \"keypad_visible\": ").append(if (keypadVisible) "true" else "false").append("\n")
            append("}\n")
        }
        return runCatching { f.writeText(json); true }.getOrDefault(false)
    }

    private fun jsonStr(s: String): String =
        '"' + s.replace("\\", "\\\\").replace("\"", "\\\"") + '"'
}

/** The JAR stem used for both the config and RMS dirs ("Foo Game.jar" → "Foo Game"). */
fun jarStem(jar: File): String = jar.nameWithoutExtension

/**
 * Discover the keypad layout names the runtime can use. Looks under the
 * runtime's expected assets path (relative to the resolved runtime
 * binary). Returns ``["minimal", "numpad"]`` as a fallback when the
 * scan fails so the dropdown isn't empty on a fresh checkout.
 */
fun discoverKeypadLayouts(): List<String> {
    val runtime = LaunchRuntime.findRuntime() ?: return defaultLayouts()
    // runtime is typically at <repo>/runtime/build/j2me. Layouts live
    // at <repo>/runtime/assets/keypad/layouts.
    val candidates = listOf(
        runtime.parentFile?.parentFile?.resolve("assets/keypad/layouts"),
        runtime.parentFile?.resolve("assets/keypad/layouts"),
        File("runtime/assets/keypad/layouts"),
    )
    val dir = candidates.firstOrNull { it != null && it.isDirectory } ?: return defaultLayouts()
    val found = dir.listFiles { f -> f.isDirectory && File(f, "layout.json").isFile }
        ?.map { it.name }
        ?.sorted()
        .orEmpty()
    return if (found.isEmpty()) defaultLayouts() else found
}

private fun defaultLayouts() = listOf("minimal", "numpad")
