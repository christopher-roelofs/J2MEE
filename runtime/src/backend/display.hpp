#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

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

    bool is_open() const { return m_window != nullptr; }

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

    void update_key_states();
    void enqueue_key(SDL_Keycode sym);
    void enqueue_release(SDL_Keycode sym);
};
