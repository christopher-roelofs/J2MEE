package org.j2me.launcher

import java.io.File

/**
 * Resolution + invocation of the native j2me runtime binary.
 *
 * We spawn the runtime as a subprocess and redirect its stdio to ours so the
 * launcher window keeps working (JVM main thread unblocked) while the game
 * runs in its own SDL window. On exit the subprocess goes away and we return
 * to the library list — no cleanup to do.
 */
object LaunchRuntime {
    /**
     * Find the built j2me binary. Looks in the obvious places relative to
     * this repo layout; returns the first that exists. Falls back to the
     * PATH-resolved `j2me` name so a globally-installed build works too.
     *
     * Override with the J2ME_RUNTIME environment variable for ad-hoc testing
     * against a specific build (useful when you have both a Release and a
     * Debug+ASan binary sitting around).
     */
    fun findRuntime(): File? {
        System.getenv("J2ME_RUNTIME")?.let {
            val f = File(it)
            if (f.canExecute()) return f
        }
        val candidates = listOf(
            // Repo-relative (launcher/.. is j2me root)
            "../runtime/build/j2me",
            "../runtime/build-release/j2me",
            // CWD-relative fallbacks
            "runtime/build/j2me",
            "runtime/build-release/j2me",
        )
        for (c in candidates) {
            val f = File(c).canonicalFile
            if (f.isFile && f.canExecute()) return f
        }
        // Last resort: let the OS resolve it from PATH
        return File("j2me")
    }

    /**
     * Spawn the runtime for [game]. Returns the started [Process], already
     * running. stdout + stderr are inherited so any runtime output (including
     * the stub-registry dump on exit) shows up in the terminal that started
     * the launcher.
     *
     * ASAN_OPTIONS=detect_leaks=0 mirrors the invocation pattern the runtime
     * is tested with — harmless on Release builds, silences known leak
     * reports on Debug+ASan builds.
     */
    fun launch(game: GameEntry): Process {
        val runtime = findRuntime()
            ?: error("could not locate j2me runtime binary (set J2ME_RUNTIME)")
        val cmd = listOf(
            runtime.absolutePath,
            game.jarPath.absolutePath,
            game.midletClass,
        )
        // Run the runtime with its own directory as cwd. Asset roots
        // (runtime/assets/keypad/...) are searched with cwd-relative
        // paths like "../assets/..." — those only resolve correctly if
        // cwd is the runtime binary's directory, not the launcher's.
        return ProcessBuilder(cmd)
            .redirectOutput(ProcessBuilder.Redirect.INHERIT)
            .redirectError(ProcessBuilder.Redirect.INHERIT)
            .directory(runtime.parentFile)
            .apply { environment()["ASAN_OPTIONS"] = "detect_leaks=0" }
            .start()
    }
}
