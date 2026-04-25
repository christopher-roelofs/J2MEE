package com.j2me.runtime

import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
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

        // Render edge-to-edge under the nav bar / cutout. Without this, SDL
        // reports the full display size but the bottom ~48dp is hidden by
        // the system nav bar — the keypad overlay ends up partially below it.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        hideSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { c ->
                c.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                c.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_FULLSCREEN
            )
        }
    }
}
