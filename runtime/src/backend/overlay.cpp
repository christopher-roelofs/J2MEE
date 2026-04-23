#include "overlay.hpp"

#include <SDL_image.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "json.hpp"

// Provided by midp/graphics_natives.cpp — same font pipeline the game uses.
// bold=true for chunky button labels.
TTF_Font* get_ttf_font(int px_size, bool bold, bool mono);
// Icon font (Font Awesome Free Solid). Returns nullptr if the TTF wasn't
// found — callers fall back to the regular text font with ASCII labels.
TTF_Font* get_icon_font(int px_size);

// Font Awesome 6 glyph codepoints we use. Names match fontawesome.com so
// they're easy to look up if we swap icons later. PUA codepoints encoded
// as UTF-8 byte strings for use with TTF_RenderUTF8_*.
namespace fa {
constexpr const char* kArrowUp    = "\xEF\x81\xA2";  // U+F062
constexpr const char* kArrowDown  = "\xEF\x81\xA3";  // U+F063
constexpr const char* kArrowLeft  = "\xEF\x81\xA0";  // U+F060
constexpr const char* kArrowRight = "\xEF\x81\xA1";  // U+F061
constexpr const char* kChevronUp  = "\xEF\x81\xB7";  // U+F077
constexpr const char* kChevronDown= "\xEF\x81\xB8";  // U+F078
constexpr const char* kRotate     = "\xEF\x80\xA1";  // U+F021  (sync/refresh)
constexpr const char* kPlay       = "\xEF\x81\x8B";  // U+F04B
constexpr const char* kAsterisk   = "\xEF\x81\xA9";  // U+F069  (we use "*" text anyway)
}

namespace {

// Below-mode colours — opaque panel, high contrast.
constexpr SDL_Color kPanelBelow        = { 18,  18,  20, 255 };
constexpr SDL_Color kBtnBelow          = { 48,  48,  54, 255 };
constexpr SDL_Color kBtnBelowEdge      = {150, 150, 160, 255 };

// Overlay-mode colours — translucent, visible-but-unobtrusive.
constexpr SDL_Color kPanelOver         = {  0,   0,   0, 130 };
constexpr SDL_Color kBtnOver           = { 40,  40,  40, 200 };
constexpr SDL_Color kBtnOverEdge       = {200, 200, 200, 200 };

constexpr SDL_Color kLabel             = {255, 255, 255, 255 };

void fill_rgba(SDL_Surface* dst, const SDL_Rect& r, SDL_Color c) {
    // Opaque fast path — SDL_FillRect is much cheaper than per-pixel blend.
    if (c.a == 255) {
        uint32_t packed = SDL_MapRGBA(dst->format, c.r, c.g, c.b, 255);
        SDL_FillRect(dst, const_cast<SDL_Rect*>(&r), packed);
        return;
    }
    // Translucent fill onto an ARGB8888 surface — per-pixel straight alpha.
    SDL_LockSurface(dst);
    uint32_t* px = static_cast<uint32_t*>(dst->pixels);
    int pitch_px = dst->pitch / 4;
    int x0 = std::max(0, r.x);
    int y0 = std::max(0, r.y);
    int x1 = std::min(dst->w, r.x + r.w);
    int y1 = std::min(dst->h, r.y + r.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint32_t p = px[y * pitch_px + x];
            uint8_t dr = (p >> 16) & 0xFF;
            uint8_t dg = (p >>  8) & 0xFF;
            uint8_t db = (p      ) & 0xFF;
            int a = c.a;
            dr = (uint8_t)((c.r * a + dr * (255 - a)) / 255);
            dg = (uint8_t)((c.g * a + dg * (255 - a)) / 255);
            db = (uint8_t)((c.b * a + db * (255 - a)) / 255);
            px[y * pitch_px + x] = 0xFF000000u | (dr << 16) | (dg << 8) | db;
        }
    }
    SDL_UnlockSurface(dst);
}

void stroke_rect(SDL_Surface* dst, const SDL_Rect& r, SDL_Color c) {
    fill_rgba(dst, {r.x,           r.y,           r.w, 1  }, c);
    fill_rgba(dst, {r.x,           r.y + r.h - 1, r.w, 1  }, c);
    fill_rgba(dst, {r.x,           r.y,           1,   r.h}, c);
    fill_rgba(dst, {r.x + r.w - 1, r.y,           1,   r.h}, c);
}

void draw_label(SDL_Surface* dst, const SDL_Rect& r, const char* text) {
    if (!text || !*text) return;
    // Icon-font glyphs live at U+F000-U+F8FF (Private Use Area). In UTF-8
    // those all start with 0xEF. Anything else is regular text.
    bool is_icon = ((uint8_t)text[0] == 0xEF);
    // Icons look best a bit smaller than the button (less visual weight
    // than same-size text), but never below 12 px or they blur.
    int px = is_icon
        ? std::max(12, (int)(r.h * 0.55))
        : std::max(10, (int)(r.h * 0.5));
    TTF_Font* font = is_icon
        ? get_icon_font(px)
        : get_ttf_font(px, /*bold*/ true, /*mono*/ false);
    // Fall back to the text font if the icon font didn't load (missing
    // file, bundled wrong, etc.) — the PUA codepoint will render as a
    // missing-glyph box but at least nothing crashes.
    if (!font)
        font = get_ttf_font(px, /*bold*/ true, /*mono*/ false);
    if (!font) return;
    SDL_Surface* txt = TTF_RenderUTF8_Blended(font, text, kLabel);
    if (!txt) return;
    SDL_Rect d{
        r.x + (r.w - txt->w) / 2,
        r.y + (r.h - txt->h) / 2,
        txt->w, txt->h,
    };
    SDL_SetSurfaceBlendMode(txt, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(txt, nullptr, dst, &d);
    SDL_FreeSurface(txt);
}

bool rect_contains(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Stable per-midp-code filenames. Used for both loading custom PNGs and
// for writing defaults via --export-keypad. Returns "" for codes we don't
// recognise (caller then falls back to live TTF rendering).
std::string button_slug(int midp_code) {
    switch (midp_code) {
        case -1: return "arrow-up";
        case -2: return "arrow-down";
        case -3: return "arrow-left";
        case -4: return "arrow-right";
        case -5: return "start";      // FIRE / OK
        case -6: return "soft-left";  // A
        case -7: return "soft-right"; // B
        case 42: return "asterisk";
        case 35: return "pound";
    }
    if (midp_code >= '0' && midp_code <= '9') {
        char c = (char)midp_code;
        return std::string("num-") + c;
    }
    return {};
}

// Root directory that the runtime looks in for default button PNGs.
// Searched in this order; first hit wins. On Android we prepend the
// runtime-resolved extracted-asset path (J2ME_ASSET_DIR from the
// bootstrap in android_platform.cpp); see kKeypadAssetRoots_storage.
const char* const kKeypadAssetRootsStatic[] = {
    "runtime/assets/keypad/default",     // invoked from repo root
    "../assets/keypad/default",          // invoked from runtime/build/
    "assets/keypad/default",             // installed layout (cwd-relative)
    "/fonts/keypad/default",             // emscripten MEMFS (future use)
};

// Root directories the runtime scans for layout.json files. First match
// wins; duplicate-named layouts in later dirs are silently skipped.
const char* const kLayoutRootsStatic[] = {
    "runtime/assets/keypad/layouts",     // invoked from repo root
    "../assets/keypad/layouts",          // invoked from runtime/build/
    "assets/keypad/layouts",             // installed layout (cwd-relative)
    "/fonts/keypad/layouts",             // emscripten MEMFS (future use)
};

// Build the live search lists, prepending the Android extracted-asset
// path when J2ME_ASSET_DIR is set. Called lazily so the env var set by
// j2me_android_bootstrap is already live.
static std::vector<std::string> build_roots(const char* const* base,
                                            size_t base_n,
                                            const char* subdir) {
    std::vector<std::string> out;
    if (const char* ad = std::getenv("J2ME_ASSET_DIR"))
        out.push_back(std::string(ad) + "/keypad/" + subdir);
    for (size_t i = 0; i < base_n; ++i) out.emplace_back(base[i]);
    return out;
}

static const std::vector<std::string>& keypad_asset_roots() {
    static const std::vector<std::string> v = build_roots(
        kKeypadAssetRootsStatic,
        sizeof(kKeypadAssetRootsStatic)/sizeof(kKeypadAssetRootsStatic[0]),
        "default");
    return v;
}
static const std::vector<std::string>& layout_roots() {
    static const std::vector<std::string> v = build_roots(
        kLayoutRootsStatic,
        sizeof(kLayoutRootsStatic)/sizeof(kLayoutRootsStatic[0]),
        "layouts");
    return v;
}

bool dir_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Load <slug>.png. If layout_dir is non-empty it's checked first (letting
// a layout override a shared default). Then the default asset roots are
// searched. The loaded image is scaled to fit the (w, h) rect preserving
// its own aspect ratio — the rest of the rect is filled with the panel
// color so non-square buttons (e.g. Start at 60×28) don't distort the
// artwork. nullptr = PNG not found.
SDL_Surface* load_png_scaled(const std::string& layout_dir,
                             const std::string& slug, int w, int h) {
    if (slug.empty() || w <= 0 || h <= 0) return nullptr;
    SDL_Surface* src = nullptr;
    char path[512];
    if (!layout_dir.empty()) {
        std::snprintf(path, sizeof(path), "%s/%s.png",
                      layout_dir.c_str(), slug.c_str());
        src = IMG_Load(path);
    }
    if (!src) {
        for (const std::string& root : keypad_asset_roots()) {
            std::snprintf(path, sizeof(path), "%s/%s.png", root.c_str(), slug.c_str());
            src = IMG_Load(path);
            if (src) break;
        }
    }
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(
        0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst) { SDL_FreeSurface(src); return nullptr; }
    // Fill the full rect with the panel colour first. If src aspect !=
    // dst aspect the uniform-scaled image leaves bands around it; those
    // need to look like the button body, not transparent.
    fill_rgba(dst, SDL_Rect{0, 0, w, h}, kBtnBelow);
    // Uniform-scale src into dst, centered. The scale is the smaller of
    // the two dimensions' ratios so nothing overflows.
    double sx = (double)w / src->w;
    double sy = (double)h / src->h;
    double s  = std::min(sx, sy);
    int iw = std::max(1, (int)std::lround(src->w * s));
    int ih = std::max(1, (int)std::lround(src->h * s));
    SDL_Rect fit{ (w - iw) / 2, (h - ih) / 2, iw, ih };
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_BlitScaled(src, nullptr, dst, &fit);
    SDL_FreeSurface(src);
    return dst;
}

// Parse one layout.json into a LayoutDef. Returns false on malformed
// input — logs a diagnostic and the layout is skipped.
bool load_layout_json(const std::string& layout_dir, LayoutDef& out) {
    std::string path = layout_dir + "/layout.json";
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto j = nlohmann::json::parse(ss.str());
        out.dir       = layout_dir;
        out.name      = j.value("name", "unnamed");
        out.strip_w   = j.value("strip_width",  176);
        out.strip_h   = j.value("strip_height",  89);
        out.strip_height_px     = j.value("strip_height_px", 0);
        out.collapsed_height_px = j.value("collapsed_height_px", 0);
        out.strip_aspect_wh     = 0.f;
        if (j.contains("strip_aspect")) {
            // "W:H" → width/height ratio. Tolerate decimals ("3:1.5") for
            // ergonomics, though most authors will stick to integers.
            std::string a = j["strip_aspect"].get<std::string>();
            float aw = 0, ah = 0;
            if (std::sscanf(a.c_str(), "%f:%f", &aw, &ah) == 2 && ah > 0)
                out.strip_aspect_wh = aw / ah;
        }
        out.buttons.clear();
        if (!j.contains("buttons") || !j["buttons"].is_array()) {
            std::fprintf(stderr, "[layout] %s: missing buttons array\n", path.c_str());
            return false;
        }
        for (const auto& b : j["buttons"]) {
            ButtonDef bd;
            // Accept either "codes": [...] (multi-key dispatch, used for
            // diagonals) or "code": N (single-key shorthand). Both styles
            // normalise to the same ButtonDef::codes vector.
            if (b.contains("codes") && b["codes"].is_array()) {
                for (const auto& c : b["codes"])
                    bd.codes.push_back(c.get<int>());
            } else {
                int c = b.value("code", 0);
                if (c != 0) bd.codes.push_back(c);
            }
            bd.image = b.value("image", std::string{});
            bd.x = b.value("x", 0.0f);
            bd.y = b.value("y", 0.0f);
            bd.w = b.value("w", 0.0f);
            bd.h = b.value("h", 0.0f);
            out.buttons.push_back(std::move(bd));
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[layout] %s: %s\n", path.c_str(), e.what());
        return false;
    }
}

// Scan runtime/assets/keypad/layouts/*/layout.json and return a list of
// loaded layouts. Order: lexicographic by folder name so the cycle is
// stable across runs.
std::vector<LayoutDef> scan_layouts() {
    // Find the first layouts root that exists. Second-level roots aren't
    // merged — users who want to ship extra layouts drop them into the
    // primary one.
    std::string chosen_root;
    for (const std::string& r : layout_roots()) {
        if (dir_exists(r)) { chosen_root = r; break; }
    }
    std::vector<LayoutDef> out;
    if (chosen_root.empty()) {
        std::fprintf(stderr, "[layout] no layouts root found; keypad will be empty\n");
        return out;
    }
    DIR* d = ::opendir(chosen_root.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (auto* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string sub = chosen_root + "/" + e->d_name;
        if (!dir_exists(sub)) continue;
        if (!file_exists(sub + "/layout.json")) continue;
        names.push_back(sub);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& p : names) {
        LayoutDef def;
        if (load_layout_json(p, def)) out.push_back(std::move(def));
    }
    return out;
}

} // namespace

Overlay::Overlay() = default;
Overlay::~Overlay() {
    clear_sprites();
    if (m_spr_toggle_hide) SDL_FreeSurface(m_spr_toggle_hide);
    if (m_spr_toggle_show) SDL_FreeSurface(m_spr_toggle_show);
    if (m_spr_cycle)       SDL_FreeSurface(m_spr_cycle);
}

int Overlay::visible_strip_height() const {
    if (m_placement != OverlayPlacement::Below) return 0;
    if (m_visible) return m_h;
    // Hidden: auto-derived from the toggle's visual bottom + a 2 px
    // buffer. A layout can override via collapsed_height_px — we clamp
    // below to at least the toggle-bottom so the chevron never gets cut.
    int derived = m_toggle_rect.y + m_toggle_rect.h + 2;
    if (m_layout_index >= 0 && m_layout_index < (int)m_layouts.size()) {
        int overr = m_layouts[m_layout_index].collapsed_height_px;
        if (overr > 0) return std::max(overr, derived);
    }
    return derived;
}

// Sizing helpers for the top control row. Both pick_strip_height (called
// from configure) and rebuild() must use the SAME values, otherwise the
// strip height accounts for one control-row size but the keypad gets
// laid out with a different one — which makes sy diverge from sx and
// the d-pad arrows come out vertically stretched.
//
// All inputs are in game-width logical pixels so the values are stable
// across configure() / rebuild() / cycle_layout(); they don't depend on
// the strip height (which would create a chicken-and-egg).
namespace ctrl {
    static int visual_side(int game_w) { return std::max(14, game_w / 12); }
    static int hit_side(int game_w)    { return std::max(28, visual_side(game_w) * 2); }
    static int gap()                   { return 4; }
    static int top_pad()               { return 2; }
    // y at which the keypad area starts inside the strip, in overlay-local
    // coords (origin_y + this).
    static int row_bottom(int game_w)  {
        return top_pad() + hit_side(game_w) + std::max(2, gap());
    }
}

// Pick the runtime strip height for a given layout using the documented
// precedence: absolute > aspect ratio > fallback-preserves-layout-aspect.
static int pick_strip_height(const LayoutDef* L, int game_w, int /*game_h*/) {
    // Keypad area height that makes sy exactly equal sx at rebuild time:
    // keypad_h / layout.strip_h == game_w / layout.strip_w. Total strip
    // = keypad_h + control row consumption (computed identically in
    // rebuild()).
    int ref_keypad_h = game_w / 2;
    if (L && L->strip_w > 0 && L->strip_h > 0) {
        ref_keypad_h = (int)std::lround(
            (double)game_w * (double)L->strip_h / (double)L->strip_w);
    }
    int fallback = std::max(ref_keypad_h + ctrl::row_bottom(game_w), 110);
    if (!L) return fallback;
    if (L->strip_height_px > 0) return L->strip_height_px;
    if (L->strip_aspect_wh > 0.f) {
        // strip_aspect is width:height of the *total* strip (game_w : h).
        // Users who set this take responsibility for the control-row
        // portion not coming out of the keypad area.
        int h = (int)std::lround(game_w / L->strip_aspect_wh);
        if (h > 0) return h;
    }
    return fallback;
}

void Overlay::configure(int gw, int gh, OverlayPlacement p) {
    m_game_w    = std::max(1, gw);
    m_game_h    = std::max(1, gh);
    m_placement = p;
    // Scan the layouts dir once per process — layouts are defined on
    // disk, not at call sites, so there's nothing to re-scan between
    // configure() invocations.
    if (m_layouts.empty()) {
        m_layouts = scan_layouts();
        m_layout_count = (int)m_layouts.size();
        if (m_layout_index >= m_layout_count) m_layout_index = 0;
        // Apply any deferred config-file preferences now that the
        // layout list is known. prefer_layout_by_name("minimal") set
        // from main.cpp will pick the index that matches, etc.
        if (!m_pending_layout.empty()) {
            for (size_t i = 0; i < m_layouts.size(); ++i) {
                auto pos = m_layouts[i].dir.find_last_of("/\\");
                std::string base = pos == std::string::npos
                    ? m_layouts[i].dir
                    : m_layouts[i].dir.substr(pos + 1);
                if (base == m_pending_layout ||
                    m_layouts[i].name == m_pending_layout) {
                    m_layout_index = (int)i;
                    break;
                }
            }
        }
        if (m_pending_visible_set) m_visible = m_pending_visible;
    }
    m_w = m_game_w;
    if (p == OverlayPlacement::Below) {
        const LayoutDef* L = (m_layout_index >= 0 &&
                              m_layout_index < (int)m_layouts.size())
            ? &m_layouts[m_layout_index] : nullptr;
        m_h = pick_strip_height(L, m_game_w, m_game_h);
    } else {
        // Translucent overlay sits in the bottom ~2/5 of the game area.
        m_h = std::min(m_game_h * 2 / 5, std::max(96, m_game_h / 3));
    }
    rebuild();
}

bool Overlay::set_layout_by_name(const std::string& name) {
    if (name.empty() || m_layouts.empty()) return false;
    // Match either against the layout folder name (last path component
    // of m_layouts[i].dir) or the human-readable name from layout.json.
    auto basename = [](const std::string& p) {
        auto pos = p.find_last_of("/\\");
        return pos == std::string::npos ? p : p.substr(pos + 1);
    };
    for (size_t i = 0; i < m_layouts.size(); ++i) {
        if (basename(m_layouts[i].dir) == name || m_layouts[i].name == name) {
            set_layout_index((int)i);
            return true;
        }
    }
    return false;
}

void Overlay::set_layout_index(int idx) {
    if (m_layout_count <= 0) return;
    // Wrap defensively — callers shouldn't have to clamp.
    m_layout_index = ((idx % m_layout_count) + m_layout_count) % m_layout_count;
    // Layouts can declare their own strip height (absolute, aspect, or
    // fallback) — recompute here so cycling swaps geometry, not just
    // button positions. Overlay-placement ignores this (fixed bottom band).
    if (m_placement == OverlayPlacement::Below) {
        const LayoutDef* L = &m_layouts[m_layout_index];
        m_h = pick_strip_height(L, m_game_w, m_game_h);
    }
    rebuild();
}

const char* Overlay::next_layout_label() const {
    // FA rotate/refresh glyph — reads as "cycle to next layout"
    // regardless of how many layouts exist. Once JSON layouts land,
    // this could show the incoming layout's short name instead.
    return fa::kRotate;
}

void Overlay::clear_sprites() {
    for (auto& b : m_buttons) {
        if (b.sprite) SDL_FreeSurface(b.sprite);
        b.sprite = nullptr;
    }
}

SDL_Surface* Overlay::bake_sprite(const SDL_Rect& r, int midp_code,
                                  const char* label) const {
    // PNG-first path: look for runtime/assets/keypad/default/<slug>.png
    // and scale-blit it to the target rect. Source dimensions don't have
    // to match anything — SDL stretches to fit.
    // Fallback bake path is still used by JSON-driven layouts when the
    // layout-local and default-root PNGs are both missing. In that case
    // the JSON's `image` slug is passed as the label so a sensible ASCII
    // string is rendered. The slug-from-code path stays as a last-resort
    // for any legacy caller that doesn't pass an image slug.
    if (SDL_Surface* png = load_png_scaled(/*layout_dir*/ {},
                                           button_slug(midp_code), r.w, r.h))
        return png;

    // Fallback: render the glyph live via TTF into a fresh ARGB surface.
    // This is what shipped before the PNG loader; kept so builds without
    // assets still render something sensible, and so the export path can
    // seed the PNGs.
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(
        0, r.w, r.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;

    bool below = (m_placement == OverlayPlacement::Below);
    SDL_Color fill   = below ? kBtnBelow     : kBtnOver;
    SDL_Color stroke = below ? kBtnBelowEdge : kBtnOverEdge;

    SDL_Rect full{0, 0, r.w, r.h};
    fill_rgba(s, full, fill);
    stroke_rect(s, full, stroke);
    draw_label(s, full, label);
    return s;
}


void Overlay::rebuild() {
    clear_sprites();
    m_buttons.clear();

    // Origin of the laid-out region. For Below, buttons are in overlay-local
    // coords (0..m_w, 0..m_h). For Overlay, we lay out inside the game's
    // bottom strip, so everything is offset by (0, game_h - strip_h).
    int origin_y = (m_placement == OverlayPlacement::Below)
        ? 0
        : (m_game_h - m_h);
    int panel_h = m_h;

    // Top-of-strip controls: compact visual squares with generously
    // expanded hit rects. The visual is top-aligned inside the hit rect
    // so when the keypad is hidden, the collapsed strip can clip right
    // below the visual (dead space below the chevron is then just
    // unreachable hit area, not visible black bars).
    //
    //   ┌───────── hit ──────────┐
    //   │   ┌── visual ──┐       │  visual hugs the top-left of the hit
    //   │   │    ▲       │       │  rect so the strip ends at visual_y
    //   │   └────────────┘       │  + visual_h when collapsed.
    //   │                        │
    //   └────────────────────────┘
    // Use the shared `ctrl::` helpers so pick_strip_height (called from
    // configure) and rebuild() agree exactly. If they disagree, the
    // keypad area's aspect drifts from the layout reference and buttons
    // come out non-square.
    int visual_side = ctrl::visual_side(m_game_w);
    int hit_side    = ctrl::hit_side(m_game_w);
    int gap         = ctrl::gap();

    int hit_group_w  = hit_side * 2 + gap;
    int hit_group_x  = (m_w - hit_group_w) / 2;
    int ctrl_y       = origin_y + ctrl::top_pad();
    m_toggle_hit = SDL_Rect{ hit_group_x, ctrl_y, hit_side, hit_side };
    m_layout_hit = SDL_Rect{ hit_group_x + hit_side + gap, ctrl_y,
                             hit_side, hit_side };

    // Visual rects: horizontally centered inside the hit rect but
    // top-aligned vertically (small inset from the strip top).
    int h_inset   = (hit_side - visual_side) / 2;
    int top_inset = std::max(2, (hit_side - visual_side) / 4);
    m_toggle_rect = SDL_Rect{
        m_toggle_hit.x + h_inset, m_toggle_hit.y + top_inset,
        visual_side, visual_side,
    };
    m_layout_rect = SDL_Rect{
        m_layout_hit.x + h_inset, m_layout_hit.y + top_inset,
        visual_side, visual_side,
    };
    int ctrl_bottom = origin_y + ctrl::row_bottom(m_game_w);

    // Keypad region is the strip area below the control row. Buttons from
    // the current layout are scaled from the layout's reference coords
    // (layout.json's strip_width × strip_height) into this region, then
    // baked as sprites. If no layout was loaded we leave m_buttons empty
    // — the user still has the top-row controls and the game is fine.
    if (m_layout_index < 0 || m_layout_index >= (int)m_layouts.size()) return;
    const LayoutDef& L = m_layouts[m_layout_index];
    if (L.strip_w <= 0 || L.strip_h <= 0) return;

    int keypad_top = ctrl_bottom;
    int keypad_h   = (origin_y + panel_h) - keypad_top;
    if (keypad_h <= 0) return;
    float sx = (float)m_w     / (float)L.strip_w;
    float sy = (float)keypad_h / (float)L.strip_h;

    for (const auto& def : L.buttons) {
        if (def.codes.empty()) continue;
        SDL_Rect rect{
            (int)(def.x * sx),
            keypad_top + (int)(def.y * sy),
            (int)(def.w * sx),
            (int)(def.h * sy),
        };
        if (rect.w <= 0 || rect.h <= 0) continue;
        // Try the layout folder first, then the default asset root; if
        // nothing matches, bake_sprite falls back to a live TTF render
        // using the image slug as the visible label. The primary code
        // (def.codes[0]) seeds the slug fallback inside bake_sprite.
        SDL_Surface* spr = load_png_scaled(L.dir, def.image, rect.w, rect.h);
        if (!spr) spr = bake_sprite(rect, def.codes.front(),
                                    def.image.c_str());
        m_buttons.push_back(Button{ rect, /*label*/ nullptr,
                                    def.codes, spr });
    }
}

OverlayHit Overlay::hit_test(int x, int y) const {
    // Keypad buttons: hit rect == the full visible rect. Adjacent d-pad
    // cells touch by design, so a click on a button always lands on a
    // button. We iterate in insertion order, so on the 1-pixel shared
    // boundary the earlier button wins — which is deterministic and
    // doesn't surprise the user.

    // When hidden, the whole thin strip is a toggle hit zone — any tap
    // there summons the keypad back. We don't know the strip's actual
    // rendered height here (Display chooses it), so treat the entire
    // overlay-local y < visible_strip_height as the toggle region.
    if (!m_visible) {
        int strip_h = visible_strip_height();
        if (y >= 0 && y < strip_h && x >= 0 && x < m_w)
            return { OverlayHit::ToggleVisible, nullptr };
        return { OverlayHit::None, nullptr };
    }

    // Visible: the toggle + layout-cycle hit rects are smaller, centered
    // on their visuals, so clicks elsewhere in the strip top don't steal
    // keypad taps.
    if (rect_contains(m_toggle_hit, x, y))
        return { OverlayHit::ToggleVisible, nullptr };
    if (rect_contains(m_layout_hit, x, y))
        return { OverlayHit::SwapLayout, nullptr };

    for (const auto& b : m_buttons) {
        if (rect_contains(b.rect, x, y))
            return { OverlayHit::Press, &b.codes };
    }
    return { OverlayHit::None, nullptr };
}

void Overlay::render_button(SDL_Surface* surface,
                            const SDL_Rect& r,
                            const char* label) const {
    bool below = (m_placement == OverlayPlacement::Below);
    fill_rgba(surface, r, below ? kBtnBelow     : kBtnOver);
    stroke_rect(surface, r, below ? kBtnBelowEdge : kBtnOverEdge);
    draw_label(surface, r, label);
}

void Overlay::render(SDL_Surface* surface) const {
    if (!surface) return;
    if (m_placement == OverlayPlacement::Below) {
        // Fill the whole strip so the background is deterministic — callers
        // don't need to clear beforehand. Drawn even when hidden so the
        // toggle chevron has a background to sit on.
        SDL_Rect full{0, 0, surface->w, surface->h};
        fill_rgba(surface, full, kPanelBelow);
    } else if (m_visible) {
        // Translucent panel over the game's bottom strip.
        SDL_Rect strip{0, m_game_h - m_h, m_w, m_h};
        fill_rgba(surface, strip, kPanelOver);
    }
    if (m_visible) {
        // Keypad buttons: pre-baked sprites. Blit each one at its rect —
        // no per-frame fill/stroke/glyph. Since the sprite was baked into
        // a surface the exact size of the rect, its center is the rect's
        // center by construction.
        for (const auto& b : m_buttons) {
            if (!b.sprite) continue;
            SDL_Rect dst = b.rect;
            SDL_BlitSurface(b.sprite, nullptr, surface, &dst);
        }
    }
    // Top-of-strip controls: prefer PNG sprites (cached until the control
    // rect changes), fall back to live TTF rendering if the PNG assets
    // aren't present. Sprite size is keyed to m_toggle_rect — same size
    // is used for the layout-cycle (they're square siblings).
    auto ensure = [&](SDL_Surface*& slot, const char* slug) {
        if (!slot) slot = load_png_scaled(
            /*layout_dir*/ {}, slug, m_toggle_rect.w, m_toggle_rect.h);
    };
    // Reload sprites if the control rect changed (resize / layout swap).
    if (m_spr_ctrl_size.w != m_toggle_rect.w || m_spr_ctrl_size.h != m_toggle_rect.h) {
        if (m_spr_toggle_hide) { SDL_FreeSurface(m_spr_toggle_hide); m_spr_toggle_hide = nullptr; }
        if (m_spr_toggle_show) { SDL_FreeSurface(m_spr_toggle_show); m_spr_toggle_show = nullptr; }
        if (m_spr_cycle)       { SDL_FreeSurface(m_spr_cycle);       m_spr_cycle       = nullptr; }
        m_spr_ctrl_size = m_toggle_rect;
    }
    ensure(m_spr_toggle_hide, "toggle-hide");
    ensure(m_spr_toggle_show, "toggle-show");
    ensure(m_spr_cycle,       "cycle");

    auto blit_or_live = [&](SDL_Surface* sprite, const SDL_Rect& dst,
                            const char* fallback_label) {
        if (sprite) {
            SDL_Rect d = dst;
            SDL_BlitSurface(sprite, nullptr, surface, &d);
        } else {
            render_button(surface, dst, fallback_label);
        }
    };
    blit_or_live(m_visible ? m_spr_toggle_hide : m_spr_toggle_show,
                 m_toggle_rect,
                 m_visible ? fa::kChevronUp : fa::kChevronDown);
    if (m_visible && m_layout_count > 1) {
        blit_or_live(m_spr_cycle, m_layout_rect, next_layout_label());
    }
}
