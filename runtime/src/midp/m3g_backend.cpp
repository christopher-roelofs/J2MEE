// SDL+GLES1 backend for the vendored Khronos M3G reference (third_party/m3g).
//
// M3G's core is platform-agnostic (m3g_core.c), but it expects the host to
// provide:
//   1. A current GLES1 context when m3gXxx render APIs are invoked
//   2. m3ggl* helpers for native-bitmap / native-window introspection
//
// This file owns:
//   - One SDL_GLContext + EGL display/surface created on demand against the
//     SDL2 window held by Display::instance().
//   - Stubs for m3ggl* — we only ever target one fixed-size surface, so the
//     parameter queries can return canned values.
//
// What is NOT in this file (yet):
//   - The javax/microedition/m3g/* native bridge that pumps games' calls
//     into m3g_core's m3gXxx() API. Coming in a follow-up commit.

#include "m3g_core.h"

#include <SDL.h>

#include <atomic>
#include <cstdio>

namespace {

// Single GL context shared by every Graphics3D the game creates. M3G expects
// the host to make it current before calling m3gBindTarget / m3gRender.
SDL_GLContext g_gl_context = nullptr;
SDL_Window*   g_gl_window  = nullptr;
std::atomic<bool> g_initialized{false};

bool ensure_gl_context() {
    if (g_initialized.load()) return true;
    // Resolve the SDL window from Display::instance(). We can't include the
    // Display header here without pulling in SDL_ttf etc., so look it up via
    // SDL_GetWindowFromID — main.cpp opens exactly one window before any
    // M3G-using game can reach this code path.
    Uint32 id = 1;
    g_gl_window = SDL_GetWindowFromID(id);
    if (!g_gl_window) {
        std::fprintf(stderr, "[m3g] no SDL window yet — defer GL init\n");
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    g_gl_context = SDL_GL_CreateContext(g_gl_window);
    if (!g_gl_context) {
        std::fprintf(stderr, "[m3g] SDL_GL_CreateContext failed: %s\n",
                     SDL_GetError());
        return false;
    }
    g_initialized.store(true);
    std::fprintf(stderr, "[m3g] GLES1 context created\n");
    return true;
}

} // namespace

// ─── M3G platform hooks (replace m3g_android_gl.cpp) ─────────────────────────
// Signatures match those declared in M3G's m3g_gl.h. M3G calls these from its
// rendercontext / texture paths; for our purposes the backing "native" memory
// is always SDL-managed so the queries return canned values.

extern "C" M3Gbool m3gglLockNativeBitmap(M3GNativeBitmap /*bitmap*/,
                                         M3Gubyte** /*ptr*/,
                                         M3Gsizei* /*stride*/) {
    return M3G_TRUE;
}

extern "C" void m3gglReleaseNativeBitmap(M3GNativeBitmap /*bitmap*/) {}

extern "C" M3Gbool m3gglGetNativeBitmapParams(M3GNativeBitmap /*bitmap*/,
                                              M3GPixelFormat* /*format*/,
                                              M3Gint* /*width*/,
                                              M3Gint* /*height*/,
                                              M3Gint* /*pixels*/) {
    return M3G_TRUE;
}

extern "C" M3Gbool m3gglGetNativeWindowParams(M3GNativeWindow /*wnd*/,
                                              M3GPixelFormat* /*format*/,
                                              M3Gint* /*width*/,
                                              M3Gint* /*height*/) {
    return M3G_TRUE;
}

// Public C++ entry point used by the upcoming m3g native bridge.
// Returns true if a GLES1 context is current and m3gXxx() calls are safe.
bool j2me_m3g_make_current() {
    if (!ensure_gl_context()) return false;
    return SDL_GL_MakeCurrent(g_gl_window, g_gl_context) == 0;
}
