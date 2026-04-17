#pragma once
#include <SDL2/SDL.h>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

// Set from a SIGINT/SIGTERM handler to request graceful shutdown. Display::
// flush() polls this each frame and returns false if set, which routes the
// exit through the normal QuitRequest path — same as clicking the window's
// close button. Going through flush() instead of abrupt _exit() means any
// in-flight RMS save gets to finish cleanly.
extern volatile std::sig_atomic_t g_quit_requested;

// Scripted key for headless/automation runs. Injected at `press_tick`; a
// matching release is injected at `press_tick + hold_ticks`.
struct ScriptedKey {
    int midp_code;     // MIDP keyCode (e.g. -1 for UP, '2' for K2, -5 for FIRE)
    int press_tick;    // flush() counter at which to press
    int hold_ticks;    // release after this many ticks of holding
};

// ─── Display ─────────────────────────────────────────────────────────────────
// Singleton SDL2 window + renderer.
// The game renders into a fixed-size logical surface (the J2ME screen).
// flushGraphics() uploads that surface to a texture and presents it.

class Display {
public:
    static Display& instance();

    // Open the window. Called once before the game loop.
    void open(int logical_w, int logical_h, const std::string& title = "J2ME");

    // The off-screen surface the game draws into.
    SDL_Surface* screen() { return m_screen; }

    // Present the current screen to the window; poll events.
    // Returns false if the user asked to quit.
    bool flush();

    // Current J2ME key state bitmask (GameCanvas constants).
    int key_states() const { return m_key_states; }

    // Drain newly-pressed MIDP key codes (keyPressed events).
    std::vector<int> take_key_presses() {
        std::vector<int> out;
        std::swap(out, m_pending_keys);
        return out;
    }

    // Drain newly-released MIDP key codes (keyReleased events).
    std::vector<int> take_key_releases() {
        std::vector<int> out;
        std::swap(out, m_pending_releases);
        return out;
    }

    // Pointer events (x,y in logical/game coordinates)
    enum class PointerKind { Pressed, Released, Dragged };
    struct PointerEvent { PointerKind kind; int x; int y; };
    std::vector<PointerEvent> take_pointer_events() {
        std::vector<PointerEvent> out;
        std::swap(out, m_pending_pointers);
        return out;
    }

    int width()  const { return m_logical_w; }
    int height() const { return m_logical_h; }

    // In headless mode the screen surface exists but no window is created.
    bool is_open() const { return m_screen != nullptr; }

    // ── Headless / automation ────────────────────────────────────────────
    // Enable before open(). Suppresses window/renderer/event-pump and
    // replaces live SDL input with a scripted key track.
    void set_headless(bool on) { m_headless = on; }
    bool is_headless() const   { return m_headless; }
    // Cap on flush() calls before flush() returns false (0 = no cap).
    void set_max_ticks(uint64_t n) { m_max_ticks = n; }
    // Optional wall-clock cap (ms since first flush).
    void set_max_run_ms(uint64_t ms) { m_max_run_ms = ms; }
    // Optional per-flush delay (usually 0 in headless).
    void set_tick_ms(uint32_t ms) { m_tick_ms = ms; }
    // Install scripted key track. Keys are delivered in order from flush().
    void set_key_script(std::vector<ScriptedKey> s) { m_key_script = std::move(s); }
    // Write the current screen surface as a binary PPM (P6). Returns false
    // if no surface exists or the file can't be written.
    bool save_ppm(const std::string& path) const;
    // Total flush() calls so far.
    uint64_t tick_count() const { return m_tick_count; }

    ~Display();

private:
    Display() = default;
    Display(const Display&) = delete;

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    SDL_Surface*  m_screen   = nullptr;

    int m_logical_w = 240;
    int m_logical_h = 320;
    int m_mouse_range_w = 0;
    int m_mouse_range_h = 0;
    int m_key_states = 0;
    std::vector<int> m_pending_keys;
    std::vector<int> m_pending_releases;
    std::vector<PointerEvent> m_pending_pointers;
    bool m_pointer_down = false;
    double m_mouse_scale = 1.0;  // multiply SDL mouse coords by this before window-to-logical mapping

    // Headless state
    bool m_headless   = false;
    uint64_t m_max_ticks  = 0;   // 0 = unlimited
    uint64_t m_max_run_ms = 0;   // 0 = unlimited
    uint32_t m_tick_ms    = 0;
    uint64_t m_tick_count = 0;
    uint64_t m_wall_start_ms = 0;
    std::vector<ScriptedKey> m_key_script;
    size_t m_key_script_idx = 0;
    // Currently-held scripted keys (for update_key_states in headless).
    std::vector<std::pair<int,uint64_t>> m_held;  // (midp_code, release_tick)

    void update_key_states();
    void enqueue_key(SDL_Keycode sym);
    void enqueue_release(SDL_Keycode sym);
    void headless_advance();
};
