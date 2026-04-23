#pragma once
// On-screen keypad. Two orthogonal settings:
//
//   Layout    — which keys appear: Minimal (d-pad + A + B + Start) or
//               Numpad (d-pad + 0-9, *, #).
//
//   Placement — where the keypad lives relative to the game:
//               Below   → dedicated strip beneath the game area (default,
//                         matches j2me-loader on Android — no pixel is
//                         shared between game and keypad).
//               Overlay → drawn translucently on top of the game surface,
//                         for titles that use the bottom portion of the
//                         screen and would otherwise lose info behind a
//                         strip, or for aspect-sensitive games.
//
// Display owns an Overlay. Pointer events are routed through hit_test()
// before the game's pointer handler: any Press is consumed (the midp code
// is enqueued) and never reaches the game.

#include <SDL2/SDL.h>
#include <string>
#include <vector>

// Single button definition as loaded from a layout.json. Coords are in
// the layout's own reference pixel space — the runtime scales them to
// fit whatever strip size the game actually uses. `image` is a slug
// (no .png extension); the loader resolves it against the layout's own
// folder first, then the shared default/ folder.
//
// A button can dispatch one or many MIDP key codes. JSON accepts either
// `"code": -1` (single) or `"codes": [-1, -4]` (multi, used for d-pad
// diagonals that mimic j2me-loader's UP+RIGHT style dual-key press).
// The loader normalises both forms into the `codes` vector.
struct ButtonDef {
    std::vector<int> codes;
    std::string      image;
    float            x = 0, y = 0, w = 0, h = 0;
};

// A layout is a reference coord space plus a list of buttons. Name is
// for debug only; the runtime cycles through layouts by index.
//
// Runtime strip height — the total pixels the keypad takes below the
// game — is picked from (in precedence order):
//   1. strip_height_px   — absolute logical-pixel height
//   2. strip_aspect      — w:h ratio; runtime derives h = game_w / ratio
//   3. built-in fallback formula (dpad-edge + control row)
// Collapsed (hidden) height works the same way: collapsed_height_px
// wins if specified, otherwise runtime derives it from the toggle rect.
struct LayoutDef {
    std::string name;
    std::string dir;               // absolute path to layout folder, for PNG lookup
    int         strip_w = 176;     // reference design-time width
    int         strip_h = 89;      // reference design-time height (keypad area only)
    int         strip_height_px      = 0;   // 0 = unset; use aspect or fallback
    int         collapsed_height_px  = 0;   // 0 = unset; auto-derive
    float       strip_aspect_wh      = 0.f; // 0 = unset; ratio width/height
    std::vector<ButtonDef> buttons;
};

// Built-in layout names — kept as a convenience for the default pair.
// Any other layout name is referenced via its index in m_layouts.
enum class OverlayLayout {
    Minimal = 0,
    Numpad  = 1,
};

enum class OverlayPlacement {
    Below,     // strip below the game area; window is sized bigger
    Overlay,   // translucent draw over the game
};

struct OverlayHit {
    enum Kind {
        None,            // tap passed through to the game
        Press,           // key was hit; caller enqueues every code in `codes`
        ToggleVisible,   // hide/show chevron was tapped
        SwapLayout,      // layout-cycle icon was tapped
    };
    Kind kind = None;
    // On Press, points into the hit Button's codes vector (owned by
    // Overlay). Valid until the next rebuild(); callers copy the codes
    // out immediately (enqueue + track-as-held) and don't retain.
    const std::vector<int>* codes = nullptr;
};

class Overlay {
public:
    Overlay();
    ~Overlay();

    // Configure — call before allocating surfaces. Picks the overlay's
    // dimensions from the game's logical size and the chosen placement.
    void configure(int game_logical_w, int game_logical_h, OverlayPlacement p);

    // Activate a specific layout (for convenience the built-in enum is
    // accepted; it maps to the corresponding index).
    void set_layout(OverlayLayout l) { set_layout_index((int)l); }

    // Index-based API — the future JSON loader will push additional layouts
    // onto the end, and callers can still cycle through all of them.
    void set_layout_index(int idx);
    int  layout_index() const { return m_layout_index; }
    int  layout_count() const { return m_layout_count; }

    // Activate the loaded layout whose folder name matches `name`.
    // Returns true on success; false if no such layout exists or no
    // layouts are loaded yet (in which case the runtime keeps the
    // current selection).
    bool set_layout_by_name(const std::string& name);

    // Stash a desired layout name + visibility to apply during the next
    // configure() call. Used by callers that want to apply config-file
    // settings before Display::open() lazily kicks Overlay::configure().
    void prefer_layout_by_name(const std::string& name) { m_pending_layout = name; }
    void prefer_visible(bool v) { m_pending_visible = v; m_pending_visible_set = true; }

    // Advance to the next layout in the list, wrapping at the end.
    void cycle_layout() {
        if (m_layout_count <= 1) return;
        set_layout_index((m_layout_index + 1) % m_layout_count);
    }

    // Short label for the *next* layout — rendered on the cycle button.
    const char* next_layout_label() const;

    OverlayPlacement placement() const { return m_placement; }

    void set_visible(bool v) { m_visible = v; }
    void toggle_visible()    { m_visible = !m_visible; }
    bool visible() const     { return m_visible; }

    // Full strip height — the size the overlay surface is allocated at.
    // Returned even when the keypad is hidden, because the surface doesn't
    // shrink; only the visible portion does. 0 for Placement::Overlay.
    int strip_height() const {
        return m_placement == OverlayPlacement::Below ? m_h : 0;
    }
    // Height of the strip *currently shown* on screen. When the keypad is
    // hidden this collapses to just the thin band around the toggle, so
    // the game area can grow to fill what used to be keypad space.
    int visible_strip_height() const;
    int width() const { return m_w; }

    // Point-test a pointer DOWN.
    // Below mode: pass overlay-local coords (caller has already subtracted
    //   the game height from y).
    // Overlay mode: pass game-local coords.
    OverlayHit hit_test(int x, int y) const;

    // Draw the overlay onto [surface]. For Below placement the surface is
    // the dedicated strip (width × strip_height()); we paint it opaque.
    // For Overlay placement the surface is the game's own screen; we
    // alpha-blend over existing pixels.
    void render(SDL_Surface* surface) const;

private:
    struct Button {
        SDL_Rect rect;                   // in (overlay-strip | game) local coords
        const char* label;
        std::vector<int> codes;          // one or many MIDP keys dispatched
                                         // together (diagonals fire two)
        SDL_Surface* sprite = nullptr;   // baked image, owned; freed on rebuild
    };

    void rebuild();
    // Build (or rebuild) a single button's sprite image. Called from
    // rebuild() after the button's rect+label+code are set. Ownership of
    // the returned surface transfers to the Button.
    //
    // If a PNG exists at <image_root>/<slug>.png it's loaded and scaled
    // to fill the rect — source dimensions are not constrained, SDL
    // stretches any size to the target. When no PNG is present we fall
    // back to rendering the label glyph via TTF (the original path). The
    // PNG-first design lets users drop in custom button art at any size.
    SDL_Surface* bake_sprite(const SDL_Rect& r, int primary_code,
                             const char* label) const;
    // Free any baked sprites. Safe to call on a fresh Overlay.
    void clear_sprites();
    // Live button render — used for the top control row whose glyph
    // changes with state (toggle chevron flips; layout-cycle label could
    // rotate). Keypad buttons use baked sprites instead.
    void render_button(SDL_Surface* surface,
                       const SDL_Rect& r,
                       const char* label) const;

    int m_game_w = 240, m_game_h = 320;
    int m_w = 240, m_h = 0;
    // Layouts loaded from runtime/assets/keypad/layouts/*/layout.json.
    // Active layout is an index into m_layouts. Loaded lazily the first
    // time configure() runs.
    std::vector<LayoutDef> m_layouts;
    int m_layout_index = 0;
    int m_layout_count = 0;

    // Deferred config from prefer_*. Applied during configure() once the
    // layout list has been scanned. Defaults are inert (empty / unset).
    std::string m_pending_layout;
    bool        m_pending_visible     = true;
    bool        m_pending_visible_set = false;
    OverlayPlacement m_placement = OverlayPlacement::Below;
    bool m_visible               = true;

    std::vector<Button> m_buttons;

    // Visual rects for the two top-of-strip controls. Rendered at this
    // size — kept compact so they don't crowd the keypad buttons below.
    SDL_Rect m_toggle_rect{0, 0, 0, 0};
    SDL_Rect m_layout_rect{0, 0, 0, 0};
    // Hit rects for the same two controls. Larger than the visuals so a
    // slightly-off tap still registers. Computed once in rebuild().
    SDL_Rect m_toggle_hit{0, 0, 0, 0};
    SDL_Rect m_layout_hit{0, 0, 0, 0};
    // Pre-baked sprites for the two control buttons. Three possible
    // images — toggle has two states (hide/show) and layout-cycle has
    // one. Loaded lazily on first render(); resized to match the current
    // rect if it changes.
    mutable SDL_Surface* m_spr_toggle_hide = nullptr;
    mutable SDL_Surface* m_spr_toggle_show = nullptr;
    mutable SDL_Surface* m_spr_cycle       = nullptr;
    mutable SDL_Rect     m_spr_ctrl_size{0, 0, 0, 0};
};
