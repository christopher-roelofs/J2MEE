package com.j2me.runtime

import android.os.Bundle
import org.libsdl.app.SDLActivity

/**
 * Hosts the native SDL thread that runs the J2ME runtime.
 *
 * SDLActivity handles the EGL/SurfaceView wiring and calls into libmain.so's
 * SDL_main (which our C++ main.cpp provides). We only override two hooks:
 *
 *  - getLibraries(): list of .so files to dlopen. "main" is libmain.so,
 *    which links SDL2 + our runtime statically into a single shared object.
 *  - onCreate(): stash the AssetManager + filesDir path into env vars so the
 *    native bootstrap (android_platform.cpp) can extract bundled assets out
 *    of the APK on first launch and set up HOME for the existing Linux-style
 *    path code in game_config.cpp / natives.cpp.
 */
class J2meActivity : SDLActivity() {

    override fun getLibraries(): Array<String> = arrayOf("main")

    override fun onCreate(savedInstanceState: Bundle?) {
        // Exported before the native thread starts — SDL_main reads these.
        System.setProperty("j2me.files_dir", filesDir.absolutePath)
        System.setProperty("j2me.assets_ready", "false")
        super.onCreate(savedInstanceState)
    }
}
