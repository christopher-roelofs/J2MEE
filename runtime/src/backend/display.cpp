#include "display.hpp"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <chrono>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

volatile std::sig_atomic_t g_quit_requested = 0;

// ─── GameCanvas key state bits (MIDP 2.0 spec) ───────────────────────────────
// MIDP GameCanvas.getKeyStates() bitfield: each bit is 1 << Canvas.<action>.
// Canvas action constants are UP=1, LEFT=2, RIGHT=5, DOWN=6, FIRE=8,
// GAME_A=9..GAME_D=12 (JSR-118). Games AND the returned mask against
// these *_PRESSED constants, so bit positions must match exactly.
namespace Key {
    constexpr int UP    = 1 << 1;   // 0x0002
    constexpr int LEFT  = 1 << 2;   // 0x0004
    constexpr int RIGHT = 1 << 5;   // 0x0020
    constexpr int DOWN  = 1 << 6;   // 0x0040
    constexpr int FIRE  = 1 << 8;   // 0x0100
    constexpr int GAME_A= 1 << 9;   // 0x0200
    constexpr int GAME_B= 1 << 10;  // 0x0400
    constexpr int GAME_C= 1 << 11;  // 0x0800
    constexpr int GAME_D= 1 << 12;  // 0x1000
}

// ─── Singleton ────────────────────────────────────────────────────────────────

Display& Display::instance() {
    static Display d;
    return d;
}

// ─── open ─────────────────────────────────────────────────────────────────────

void Display::open(int w, int h, const std::string& title) {
    if (m_screen) return;  // already open

    m_logical_w = w;
    m_logical_h = h;

    // Configure the overlay based on the game's logical size and the user
    // preference in J2ME_OVERLAY_PLACEMENT / _LAYOUT env vars.
    //   J2ME_OVERLAY_PLACEMENT = "below" (default) | "overlay" | "off"
    //   J2ME_OVERLAY_LAYOUT    = "minimal" (default) | "numpad"
    OverlayPlacement placement = OverlayPlacement::Below;
    bool overlay_enabled = true;
    if (const char* p = std::getenv("J2ME_OVERLAY_PLACEMENT")) {
        if      (std::strcmp(p, "below"  ) == 0) placement = OverlayPlacement::Below;
        else if (std::strcmp(p, "overlay") == 0) placement = OverlayPlacement::Overlay;
        else if (std::strcmp(p, "off"    ) == 0) overlay_enabled = false;
    }
    m_overlay.configure(w, h, placement);
    if (const char* l = std::getenv("J2ME_OVERLAY_LAYOUT")) {
        if (std::strcmp(l, "numpad") == 0)
            m_overlay.set_layout(OverlayLayout::Numpad);
    }
    m_overlay.set_visible(overlay_enabled);
    const int overlay_strip = m_overlay.strip_height();
    const int total_h       = h + overlay_strip;

    // Headless: create the offscreen ARGB8888 surface the game draws into,
    // but skip SDL_Init(VIDEO), the window, the renderer, and the texture.
    // SDL_CreateRGBSurfaceWithFormat works without any subsystem init.
    if (m_headless) {
        m_screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!m_screen)
            throw std::runtime_error(std::string("SDL_CreateRGBSurface: ") + SDL_GetError());
        SDL_FillRect(m_screen, nullptr, SDL_MapRGBA(m_screen->format, 255, 255, 255, 255));
        m_wall_start_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) < 0)
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

    // Compositor-scale compensation: on GNOME/KDE Wayland with HiDPI scaling
    // (including XWayland), SDL reports window size N but mouse events arrive
    // in the post-compositor-scale coord space (N/scale). We detect this and
    // scale mouse coords back up before RenderWindowToLogical, so resizing
    // the window still works correctly.
    //
    // Override: set J2ME_SCALE=<float> (e.g. 1 to disable, 2 for 200% HiDPI).
    if (const char* env = std::getenv("J2ME_SCALE")) {
        double v = std::atof(env);
        if (v > 0.1) m_mouse_scale = v;
    } else if (std::getenv("WAYLAND_DISPLAY")) {
        m_mouse_scale = 2.0;
    }

    // Scale the window to ~75% of screen height, maintaining aspect ratio.
    // Window height includes the overlay strip so the keypad has real pixels
    // to live in beneath the game (for Placement::Below).
    SDL_DisplayMode dm;
    int win_w = w * 2, win_h = total_h * 2;  // default 2x
#ifdef __EMSCRIPTEN__
    // Under Emscripten the HTML <canvas> element is the "window". Force its
    // backing store to (game_w × (game_h + overlay_h)) so SDL draws 1:1 — the
    // HTML shell's CSS stretches the element to its fixed display size,
    // with image-rendering: pixelated keeping the upscale crisp.
    emscripten_set_canvas_element_size("#canvas", w, total_h);
    win_w = w;
    win_h = total_h;
    (void)dm;
#else
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        int max_h = (dm.h * 3) / 4;  // target 75% of screen height
        int max_w = dm.w - 80;
        double scale_h = (double)max_h / total_h;
        double scale_w = (double)max_w / w;
        double scale = std::min(scale_h, scale_w);
        if (scale < 1.0) scale = 1.0;
        win_w = (int)(w * scale);
        win_h = (int)(total_h * scale);
    }
#endif
    // SDL_WINDOW_OPENGL: M3G needs a GLES1 context to render 3D. Without the
    // flag, SDL_GL_CreateContext against this window fails with BadAlloc on
    // X11. The SDL renderer still works alongside M3G's context — they both
    // target the same window and SDL_GL_MakeCurrent selects which is active
    // at any moment.
    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        SDL_WINDOW_OPENGL);
    if (!m_window)
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());

    m_renderer = SDL_CreateRenderer(m_window, -1,
                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        // Headless browsers, WebGL-disabled contexts, and weird X11 configs
        // lack an accelerated driver; SDL's software renderer works for our
        // single upload-per-frame path.
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!m_renderer) {
        SDL_DestroyWindow(m_window);
        throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    }

    // Rendering: we manage our own per-surface dst rects in window-pixel
    // space each frame (see flush). SDL_RenderSetLogicalSize is NOT used
    // — it bakes a single logical coord space into the renderer, which
    // makes the game and the keypad share one coord system and breaks
    // mouse mapping when the keypad visibility toggles or the user
    // resizes the window. Kept out.

    // Mouse events on Wayland/XWayland arrive in a coordinate space that stays
    // fixed even when the user resizes the window. Capture that range now (it
    // equals the initial window size in "screen coords", i.e. logical
    // compositor pixels) and map clicks against it directly.
    int sdlw = 0, sdlh = 0;
    SDL_GetWindowSize(m_window, &sdlw, &sdlh);
    m_mouse_range_w = (int)(sdlw / m_mouse_scale);
    m_mouse_range_h = (int)(sdlh / m_mouse_scale);
    if (m_mouse_range_w <= 0) m_mouse_range_w = w;
    if (m_mouse_range_h <= 0) m_mouse_range_h = h;
    // Record the initial window/logical scale so we can resize the window
    // proportionally when the overlay is hidden/shown.
    m_display_scale   = (double)sdlh / (double)total_h;
    m_cached_total_h  = total_h;

    // Off-screen surface (ARGB8888 — matches what GameCanvas draws into)
    m_screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!m_screen)
        throw std::runtime_error(std::string("SDL_CreateRGBSurface: ") + SDL_GetError());

    // Fill with white (J2ME default background)
    SDL_FillRect(m_screen, nullptr, SDL_MapRGBA(m_screen->format, 255, 255, 255, 255));

    // Disable text-input mode. On Wayland, SDL enables text input by default
    // after window creation which suppresses SDL_KEYDOWN for some keys (the
    // compositor routes them to an IME text-editing stream instead). Games
    // want raw key events, not IME text — stop it up front.
    SDL_StopTextInput();

    // Streaming texture for efficient surface→GPU upload
    m_texture = SDL_CreateTexture(m_renderer,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!m_texture)
        throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
    // Don't blend texture alpha — the screen surface is the final composited image
    SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_NONE);

    // Below-placement overlay gets its own surface + streaming texture so
    // the game frame and keypad can be re-rendered independently (the keypad
    // layout doesn't change frame-to-frame). Overlay-placement reuses the
    // game surface and needs neither allocation.
    if (placement == OverlayPlacement::Below && overlay_strip > 0) {
        m_overlay_surf = SDL_CreateRGBSurfaceWithFormat(
            0, w, overlay_strip, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!m_overlay_surf)
            throw std::runtime_error(std::string("SDL_CreateRGBSurface overlay: ") + SDL_GetError());
        m_overlay_texture = SDL_CreateTexture(m_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            w, overlay_strip);
        if (!m_overlay_texture)
            throw std::runtime_error(std::string("SDL_CreateTexture overlay: ") + SDL_GetError());
        SDL_SetTextureBlendMode(m_overlay_texture, SDL_BLENDMODE_NONE);
    }
}

// ─── flush ────────────────────────────────────────────────────────────────────

bool Display::flush() {
    // Terminal signal (Ctrl+C, SIGTERM) routes through the same path as
    // window-close so that in-flight RMS saves / timers unwind cleanly.
    if (g_quit_requested) {
        fprintf(stderr, "[display] quit requested via signal\n");
        return false;
    }
    if (m_headless) {
        // Advance scripted input / tick counter, then check budgets.
        headless_advance();
        ++m_tick_count;
        if (m_max_ticks && m_tick_count >= m_max_ticks) return false;
        if (m_max_run_ms) {
            uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - m_wall_start_ms >= m_max_run_ms) return false;
        }
        if (m_tick_ms) SDL_Delay(m_tick_ms);
        return true;
    }
    if (!m_window) return true;

    // Compute per-surface destination rects in window-pixel space. Treat
    // the game and the keypad as two independent surfaces sharing the
    // window: we preserve the game's aspect (and the keypad's) by
    // choosing a single scale that fits the combined logical size into
    // the window; letterbox bars fill any remainder.
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(m_window, &win_w, &win_h);
    const int strip_vis = m_overlay.visible_strip_height();
    const int combined_w = m_logical_w;
    const int combined_h = m_logical_h + strip_vis;
    double sx = (double)win_w / combined_w;
    double sy = (double)win_h / combined_h;
    double s  = sx < sy ? sx : sy;
    int content_w = (int)(combined_w * s);
    int content_h = (int)(combined_h * s);
    int ox = (win_w - content_w) / 2;
    int oy = (win_h - content_h) / 2;
    SDL_Rect game_dst{ ox, oy, content_w, (int)(m_logical_h * s) };
    SDL_Rect kp_dst  { ox, oy + game_dst.h, content_w,
                       content_h - game_dst.h };

    // Poll events
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (std::getenv("J2ME_TRACE_KEYS"))
            fprintf(stderr, "[sdl] event type=0x%x\n", ev.type);
        if (ev.type == SDL_QUIT) { fprintf(stderr, "[display] SDL_QUIT received\n"); return false; }
        if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE) { fprintf(stderr, "[display] ESCAPE pressed\n"); return false; }
            // Tab toggles the on-screen keypad. Swallowed — Tab is rarely
            // a MIDP game input, and we don't want both things to fire.
            if (ev.key.keysym.sym == SDLK_TAB) {
                if (ev.key.repeat == 0) m_overlay.toggle_visible();
                continue;
            }
            if (ev.key.repeat == 0)  // ignore auto-repeat; only fire on initial press
                enqueue_key(ev.key.keysym.sym);
        } else if (ev.type == SDL_KEYUP) {
            if (ev.key.keysym.sym == SDLK_TAB) continue;
            enqueue_release(ev.key.keysym.sym);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            // Map the window-pixel click against whichever dst rect
            // contains it, then use that rect's own width/height to
            // convert to its surface's local coord space. Game and
            // keypad have independent ratios, so neither disturbs the
            // other when visibility or window size changes.
            int bx = ev.button.x, by = ev.button.y;
            auto map_into = [](int x, int y, const SDL_Rect& dst,
                               int logical_w, int logical_h,
                               int& out_x, int& out_y) -> bool {
                if (x < dst.x || y < dst.y ||
                    x >= dst.x + dst.w || y >= dst.y + dst.h ||
                    dst.w == 0 || dst.h == 0) return false;
                out_x = (int)((long long)(x - dst.x) * logical_w / dst.w);
                out_y = (int)((long long)(y - dst.y) * logical_h / dst.h);
                return true;
            };
            int gx = 0, gy = 0, kx = 0, ky = 0;
            if (strip_vis > 0 && map_into(bx, by, kp_dst, m_logical_w, strip_vis, kx, ky)) {
                OverlayHit h = m_overlay.hit_test(kx, ky);
                if (std::getenv("J2ME_TRACE_MOUSE")) {
                    int first = (h.codes && !h.codes->empty()) ? (*h.codes)[0] : 0;
                    int n     =  h.codes ? (int)h.codes->size() : 0;
                    fprintf(stderr, "[mouse] kp ev=(%d,%d) local=(%d,%d) kind=%d codes=%d x %d\n",
                            bx, by, kx, ky, (int)h.kind, n, first);
                }
                if (h.kind == OverlayHit::Press && h.codes) {
                    // Dispatch every code on the button simultaneously.
                    // For a diagonal button carrying (UP, RIGHT), the game
                    // sees two keyPresseds + matching two keyReleaseds on
                    // lift + the GameCanvas state mask has both bits set
                    // while held. Mirrors j2me-loader's DualKey.
                    for (int code : *h.codes) {
                        if (code == 0) continue;
                        m_pending_keys.push_back(code);
                        m_overlay_held.insert(code);
                    }
                } else if (h.kind == OverlayHit::ToggleVisible) {
                    m_overlay.toggle_visible();
                } else if (h.kind == OverlayHit::SwapLayout) {
                    m_overlay.cycle_layout();
                }
            } else if (map_into(bx, by, game_dst, m_logical_w, m_logical_h, gx, gy)) {
                if (std::getenv("J2ME_TRACE_MOUSE"))
                    fprintf(stderr, "[mouse] game ev=(%d,%d) local=(%d,%d)\n",
                            bx, by, gx, gy);
                m_pointer_down = true;
                m_pending_pointers.push_back({PointerKind::Pressed, gx, gy});
            }
        } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
            // Release any held overlay key first. We synthesise the release
            // regardless of where the finger lifted — games expect a matching
            // keyReleased for every keyPressed. Diagonals hold 2 codes.
            for (int code : m_overlay_held)
                m_pending_releases.push_back(code);
            m_overlay_held.clear();
            if (m_pointer_down) {
                // Map the release into game-local coords via the game
                // rect we already computed at the top of flush(). Clamp
                // to game bounds so the matching Released still closes
                // the Pressed even if the finger lifted outside the rect.
                int gx = 0, gy = 0;
                int bx = ev.button.x, by = ev.button.y;
                if (game_dst.w > 0 && game_dst.h > 0) {
                    gx = (int)((long long)(bx - game_dst.x) * m_logical_w / game_dst.w);
                    gy = (int)((long long)(by - game_dst.y) * m_logical_h / game_dst.h);
                }
                if (gx < 0) gx = 0; else if (gx >= m_logical_w) gx = m_logical_w - 1;
                if (gy < 0) gy = 0; else if (gy >= m_logical_h) gy = m_logical_h - 1;
                m_pending_pointers.push_back({PointerKind::Released, gx, gy});
                m_pointer_down = false;
            }
        } else if (ev.type == SDL_MOUSEMOTION && m_pointer_down) {
            int gx = 0, gy = 0;
            int bx = ev.motion.x, by = ev.motion.y;
            if (game_dst.w > 0 && game_dst.h > 0) {
                gx = (int)((long long)(bx - game_dst.x) * m_logical_w / game_dst.w);
                gy = (int)((long long)(by - game_dst.y) * m_logical_h / game_dst.h);
            }
            if (gx < 0) gx = 0; else if (gx >= m_logical_w) gx = m_logical_w - 1;
            if (gy < 0) gy = 0; else if (gy >= m_logical_h) gy = m_logical_h - 1;
            m_pending_pointers.push_back({PointerKind::Dragged, gx, gy});
        }
    }

    update_key_states();

    if (const char* dir = std::getenv("J2ME_DUMP_FRAMES")) {
        static int frame_n = 0;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/frame_%06d.bmp", dir, frame_n++);
        SDL_SaveBMP(m_screen, path);
        if (std::getenv("J2ME_TRACE_DRAW"))
            fprintf(stderr, "===== FLUSH #%d =====\n", frame_n - 1);
    }

    // Game frame → texture.
    SDL_UpdateTexture(m_texture, nullptr, m_screen->pixels, m_screen->pitch);

    SDL_RenderClear(m_renderer);

    // Blit the game into its window-pixel rect (computed at the top of
    // flush()). SDL stretches the game texture to fill the rect preserving
    // no aspect on its own — we chose game_dst with the right aspect by
    // construction. When the keypad is hidden, game_dst grew to take
    // (game_h + strip_hidden) / (game_h + strip_visible_full) more of the
    // window's vertical space.
    SDL_RenderCopy(m_renderer, m_texture, nullptr, &game_dst);

    // Keypad: overlay surface is allocated at the layout's full strip
    // height. If the active layout's strip height differs from what we
    // allocated, re-size the surface + texture. Happens on layout cycle
    // when layouts declare different strip heights.
    const int strip_full = m_overlay.strip_height();
    if (strip_full > 0 &&
        (!m_overlay_surf || !m_overlay_texture ||
         m_overlay_surf->h != strip_full || m_overlay_surf->w != m_logical_w)) {
        if (m_overlay_surf)    { SDL_FreeSurface(m_overlay_surf);   m_overlay_surf = nullptr; }
        if (m_overlay_texture) { SDL_DestroyTexture(m_overlay_texture); m_overlay_texture = nullptr; }
        m_overlay_surf = SDL_CreateRGBSurfaceWithFormat(
            0, m_logical_w, strip_full, 32, SDL_PIXELFORMAT_ARGB8888);
        if (m_overlay_surf) {
            m_overlay_texture = SDL_CreateTexture(m_renderer,
                SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                m_logical_w, strip_full);
            if (m_overlay_texture)
                SDL_SetTextureBlendMode(m_overlay_texture, SDL_BLENDMODE_NONE);
        }
    }
    if (m_overlay_surf && m_overlay_texture && strip_full > 0 && strip_vis > 0) {
        m_overlay.render(m_overlay_surf);
        SDL_UpdateTexture(m_overlay_texture, nullptr,
                          m_overlay_surf->pixels, m_overlay_surf->pitch);
        SDL_Rect ov_src { 0, 0, m_logical_w, strip_vis };
        SDL_RenderCopy(m_renderer, m_overlay_texture, &ov_src, &kp_dst);
    }
    // Overlay-placement overlays the game surface directly — nothing extra
    // to render here since the keypad was blended into m_screen already.
    // (Kept wired but unused while the launcher's UX only exposes Below.)
    else if (m_overlay.placement() == OverlayPlacement::Overlay && m_overlay.visible()) {
        // no-op: m_overlay.render(m_screen) would run here if we re-enabled
        // translucent placement. Left as a reminder.
    }

    SDL_RenderPresent(m_renderer);

    return true;
}

// ─── Key events ──────────────────────────────────────────────────────────────
// Map SDL keysyms to MIDP key codes and push onto the pending queue.
// MIDP key code conventions (CLDC 1.1 / MIDP 2.0):
//   UP=-1  DOWN=-2  LEFT=-3  RIGHT=-4  FIRE=-5
//   SOFT1=-6  SOFT2=-7
//   Digits '0'-'9' (48-57), '*'=42, '#'=35

// Override the default soft-key keycodes. Set J2ME_SOFTKEYS="-6,-7" (Nokia,
// default), "21,22" (Siemens / some Jamdat titles like Bejeweled 3), or any
// other pair your game expects.
static int g_softkey_left  = -6;
static int g_softkey_right = -7;
static bool g_softkeys_inited = false;
static void init_softkeys() {
    if (g_softkeys_inited) return;
    g_softkeys_inited = true;
    const char* env = std::getenv("J2ME_SOFTKEYS");
    if (!env) return;
    int l = 0, r = 0;
    if (sscanf(env, "%d,%d", &l, &r) == 2) {
        g_softkey_left = l;
        g_softkey_right = r;
    }
}

void Display::enqueue_key(SDL_Keycode sym) {
    init_softkeys();
    if (std::getenv("J2ME_TRACE_KEYS"))
        fprintf(stderr, "[key] enqueue_key sym=%d (%s)\n", sym, SDL_GetKeyName(sym));
    int midp = 0;
    switch (sym) {
        case SDLK_UP:        midp = -1;  break;
        case SDLK_DOWN:      midp = -2;  break;
        case SDLK_LEFT:      midp = -3;  break;
        case SDLK_RIGHT:     midp = -4;  break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
        case SDLK_z:         midp = -5;  break;  // FIRE / OK
        case SDLK_F1:
        case SDLK_LSHIFT:    midp = g_softkey_left;  break;
        case SDLK_F2:
        case SDLK_BACKSPACE:
        case SDLK_RSHIFT:    midp = g_softkey_right; break;
        case SDLK_w:         midp = -1;  break;  // WASD up
        case SDLK_s:         midp = -2;  break;
        case SDLK_a:         midp = -3;  break;
        case SDLK_d:         midp = -4;  break;
        default:
            // Digit keys 0-9
            if (sym >= SDLK_0 && sym <= SDLK_9)
                midp = static_cast<int>(sym);  // '0'=48 … '9'=57
            else if (sym >= SDLK_KP_0 && sym <= SDLK_KP_9)
                midp = static_cast<int>(sym - SDLK_KP_0 + SDLK_0);
            break;
    }
    if (midp != 0)
        m_pending_keys.push_back(midp);
}

void Display::enqueue_release(SDL_Keycode sym) {
    init_softkeys();
    // Reuse the same mapping — just push to the release queue instead
    int midp = 0;
    switch (sym) {
        case SDLK_UP:        midp = -1;  break;
        case SDLK_DOWN:      midp = -2;  break;
        case SDLK_LEFT:      midp = -3;  break;
        case SDLK_RIGHT:     midp = -4;  break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
        case SDLK_z:         midp = -5;  break;
        case SDLK_F1:
        case SDLK_LSHIFT:    midp = g_softkey_left;  break;
        case SDLK_F2:
        case SDLK_BACKSPACE:
        case SDLK_RSHIFT:    midp = g_softkey_right; break;
        case SDLK_w:         midp = -1;  break;
        case SDLK_s:         midp = -2;  break;
        case SDLK_a:         midp = -3;  break;
        case SDLK_d:         midp = -4;  break;
        default:
            if (sym >= SDLK_0 && sym <= SDLK_9)
                midp = static_cast<int>(sym);
            else if (sym >= SDLK_KP_0 && sym <= SDLK_KP_9)
                midp = static_cast<int>(sym - SDLK_KP_0 + SDLK_0);
            break;
    }
    if (midp != 0)
        m_pending_releases.push_back(midp);
}

// ─── Headless scripted input ─────────────────────────────────────────────────
// Each flush() call in headless mode = one tick. Keys with press_tick ==
// tick_count are pushed as keyPressed; held keys whose release_tick fires are
// pushed as keyReleased. update_key_states() uses m_held for getKeyStates().

void Display::headless_advance() {
    // Fire any scheduled releases first so games that poll between press and
    // release see the press-only state for at least one tick.
    for (auto it = m_held.begin(); it != m_held.end(); ) {
        if (m_tick_count >= it->second) {
            m_pending_releases.push_back(it->first);
            it = m_held.erase(it);
        } else {
            ++it;
        }
    }
    // Press any keys whose press_tick has arrived.
    while (m_key_script_idx < m_key_script.size()
           && m_key_script[m_key_script_idx].press_tick <= (int64_t)m_tick_count) {
        const ScriptedKey& k = m_key_script[m_key_script_idx++];
        m_pending_keys.push_back(k.midp_code);
        int hold = k.hold_ticks > 0 ? k.hold_ticks : 2;
        m_held.push_back({k.midp_code, m_tick_count + hold});
    }
    update_key_states();
}

// ─── Key state ────────────────────────────────────────────────────────────────

void Display::update_key_states() {
    if (m_headless) {
        // Derive state bitmask from currently-held scripted keys only.
        int bits = 0;
        for (auto& h : m_held) {
            switch (h.first) {
                case -1: bits |= Key::UP;    break;
                case -2: bits |= Key::DOWN;  break;
                case -3: bits |= Key::LEFT;  break;
                case -4: bits |= Key::RIGHT; break;
                case -5: bits |= Key::FIRE;  break;
                default: break;
            }
        }
        m_key_states = bits;
        return;
    }
    const uint8_t* kb = SDL_GetKeyboardState(nullptr);
    int bits = 0;

    if (kb[SDL_SCANCODE_UP]    || kb[SDL_SCANCODE_W]) bits |= Key::UP;
    if (kb[SDL_SCANCODE_DOWN]  || kb[SDL_SCANCODE_S]) bits |= Key::DOWN;
    if (kb[SDL_SCANCODE_LEFT]  || kb[SDL_SCANCODE_A]) bits |= Key::LEFT;
    if (kb[SDL_SCANCODE_RIGHT] || kb[SDL_SCANCODE_D]) bits |= Key::RIGHT;
    if (kb[SDL_SCANCODE_SPACE] || kb[SDL_SCANCODE_RETURN] ||
        kb[SDL_SCANCODE_Z])                            bits |= Key::FIRE;
    if (kb[SDL_SCANCODE_Q])   bits |= Key::GAME_A;
    if (kb[SDL_SCANCODE_E])   bits |= Key::GAME_B;
    if (kb[SDL_SCANCODE_1])   bits |= Key::GAME_C;
    if (kb[SDL_SCANCODE_2])   bits |= Key::GAME_D;

    // Finger-on-overlay counts too — a user holding the d-pad expects
    // GameCanvas.getKeyStates() to return the direction bit for as long
    // as they keep it pressed. Diagonals hold two codes, so OR every
    // bit independently.
    for (int code : m_overlay_held) {
        switch (code) {
            case -1: bits |= Key::UP;    break;
            case -2: bits |= Key::DOWN;  break;
            case -3: bits |= Key::LEFT;  break;
            case -4: bits |= Key::RIGHT; break;
            case -5: bits |= Key::FIRE;  break;
            default: break;
        }
    }

    m_key_states = bits;
}

// ─── PPM dump ────────────────────────────────────────────────────────────────
// Binary P6, matches brewhle's format so the same tools work on both.

bool Display::save_ppm(const std::string& path) const {
    if (!m_screen) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", m_logical_w, m_logical_h);
    SDL_LockSurface(m_screen);
    const uint32_t* px = static_cast<const uint32_t*>(m_screen->pixels);
    int pitch_px = m_screen->pitch / 4;
    for (int y = 0; y < m_logical_h; ++y) {
        for (int x = 0; x < m_logical_w; ++x) {
            uint32_t p = px[y * pitch_px + x];
            uint8_t rgb[3] = {
                (uint8_t)((p >> 16) & 0xFF),
                (uint8_t)((p >>  8) & 0xFF),
                (uint8_t)((p >>  0) & 0xFF),
            };
            std::fwrite(rgb, 1, 3, f);
        }
    }
    SDL_UnlockSurface(m_screen);
    std::fclose(f);
    return true;
}

// ─── Destructor ───────────────────────────────────────────────────────────────

Display::~Display() {
    if (m_overlay_texture) SDL_DestroyTexture(m_overlay_texture);
    if (m_overlay_surf)    SDL_FreeSurface(m_overlay_surf);
    if (m_screen)   SDL_FreeSurface(m_screen);
    if (m_texture)  SDL_DestroyTexture(m_texture);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window)   SDL_DestroyWindow(m_window);
    // In headless mode we never called SDL_Init, so skip SDL_Quit.
    if (!m_headless) SDL_Quit();
}
