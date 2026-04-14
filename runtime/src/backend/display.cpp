#include "display.hpp"
#include <stdexcept>
#include <cstdio>

// ─── GameCanvas key state bits (MIDP 2.0 spec) ───────────────────────────────
// These must match the values the game expects from getKeyStates().
namespace Key {
    constexpr int UP    = 1 << 0;   // 1
    constexpr int DOWN  = 1 << 1;   // 2
    constexpr int LEFT  = 1 << 2;   // 4
    constexpr int RIGHT = 1 << 3;   // 8
    constexpr int FIRE  = 1 << 4;   // 16
    constexpr int GAME_A= 1 << 5;   // 32
    constexpr int GAME_B= 1 << 6;   // 64
    constexpr int GAME_C= 1 << 7;   // 128
    constexpr int GAME_D= 1 << 8;   // 256
}

// ─── Singleton ────────────────────────────────────────────────────────────────

Display& Display::instance() {
    static Display d;
    return d;
}

// ─── open ─────────────────────────────────────────────────────────────────────

void Display::open(int w, int h, const std::string& title) {
    if (m_window) return;  // already open

    m_logical_w = w;
    m_logical_h = h;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) < 0)
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

    // Scale the window to ~75% of screen height, maintaining aspect ratio
    SDL_DisplayMode dm;
    int win_w = w * 2, win_h = h * 2;  // default 2x
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        int max_h = (dm.h * 3) / 4;  // target 75% of screen height
        int max_w = dm.w - 80;
        double scale_h = (double)max_h / h;
        double scale_w = (double)max_w / w;
        double scale = std::min(scale_h, scale_w);
        if (scale < 1.0) scale = 1.0;
        win_w = (int)(w * scale);
        win_h = (int)(h * scale);
    }
    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!m_window)
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());

    m_renderer = SDL_CreateRenderer(m_window, -1,
                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        SDL_DestroyWindow(m_window);
        throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    }

    // Scale rendering to the logical resolution
    SDL_RenderSetLogicalSize(m_renderer, w, h);

    // Off-screen surface (ARGB8888 — matches what GameCanvas draws into)
    m_screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!m_screen)
        throw std::runtime_error(std::string("SDL_CreateRGBSurface: ") + SDL_GetError());

    // Fill with white (J2ME default background)
    SDL_FillRect(m_screen, nullptr, SDL_MapRGBA(m_screen->format, 255, 255, 255, 255));

    // Streaming texture for efficient surface→GPU upload
    m_texture = SDL_CreateTexture(m_renderer,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!m_texture)
        throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
    // Don't blend texture alpha — the screen surface is the final composited image
    SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_NONE);
}

// ─── flush ────────────────────────────────────────────────────────────────────

bool Display::flush() {
    if (!m_window) return true;

    // Poll events
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return false;
        if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE) return false;
            if (ev.key.repeat == 0)  // ignore auto-repeat; only fire on initial press
                enqueue_key(ev.key.keysym.sym);
        } else if (ev.type == SDL_KEYUP) {
            enqueue_release(ev.key.keysym.sym);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            float lx = 0, ly = 0;
            SDL_RenderWindowToLogical(m_renderer, ev.button.x, ev.button.y, &lx, &ly);
            int ix = (int)lx, iy = (int)ly;
            if (ix >= 0 && iy >= 0 && ix < m_logical_w && iy < m_logical_h) {
                m_pointer_down = true;
                m_pending_pointers.push_back({PointerKind::Pressed, ix, iy});
            }
        } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
            if (m_pointer_down) {
                float lx = 0, ly = 0;
                SDL_RenderWindowToLogical(m_renderer, ev.button.x, ev.button.y, &lx, &ly);
                int ix = (int)lx, iy = (int)ly;
                if (ix < 0) ix = 0; else if (ix >= m_logical_w) ix = m_logical_w - 1;
                if (iy < 0) iy = 0; else if (iy >= m_logical_h) iy = m_logical_h - 1;
                m_pending_pointers.push_back({PointerKind::Released, ix, iy});
                m_pointer_down = false;
            }
        } else if (ev.type == SDL_MOUSEMOTION && m_pointer_down) {
            float lx = 0, ly = 0;
            SDL_RenderWindowToLogical(m_renderer, ev.motion.x, ev.motion.y, &lx, &ly);
            int ix = (int)lx, iy = (int)ly;
            if (ix < 0) ix = 0; else if (ix >= m_logical_w) ix = m_logical_w - 1;
            if (iy < 0) iy = 0; else if (iy >= m_logical_h) iy = m_logical_h - 1;
            m_pending_pointers.push_back({PointerKind::Dragged, ix, iy});
        }
    }

    update_key_states();

    // Upload surface pixels to texture
    SDL_UpdateTexture(m_texture, nullptr, m_screen->pixels, m_screen->pitch);

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);

    return true;
}

// ─── Key events ──────────────────────────────────────────────────────────────
// Map SDL keysyms to MIDP key codes and push onto the pending queue.
// MIDP key code conventions (CLDC 1.1 / MIDP 2.0):
//   UP=-1  DOWN=-2  LEFT=-3  RIGHT=-4  FIRE=-5
//   SOFT1=-6  SOFT2=-7
//   Digits '0'-'9' (48-57), '*'=42, '#'=35

void Display::enqueue_key(SDL_Keycode sym) {
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
        case SDLK_LSHIFT:    midp = -6;  break;  // Left soft key
        case SDLK_F2:
        case SDLK_BACKSPACE:
        case SDLK_RSHIFT:    midp = -7;  break;  // Right soft key
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
        case SDLK_LSHIFT:    midp = -6;  break;
        case SDLK_F2:
        case SDLK_BACKSPACE:
        case SDLK_RSHIFT:    midp = -7;  break;
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

// ─── Key state ────────────────────────────────────────────────────────────────

void Display::update_key_states() {
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

    m_key_states = bits;
}

// ─── Destructor ───────────────────────────────────────────────────────────────

Display::~Display() {
    if (m_screen)   SDL_FreeSurface(m_screen);
    if (m_texture)  SDL_DestroyTexture(m_texture);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window)   SDL_DestroyWindow(m_window);
    SDL_Quit();
}
