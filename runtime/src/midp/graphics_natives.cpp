#include "natives.hpp"
#include "backend/display.hpp"
#include "vm/vm.hpp"
#include "vm/heap.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <cstring>
#include <cmath>
#include <unordered_map>
#include <zlib.h>  // crc32 for JAMDAT PNG repair

// ─── External state ──────────────────────────────────────────────────────────
// Graphics objects draw into SDL surfaces.  The "main" graphics always draws
// into Display::instance().screen().  Off-screen Image surfaces are kept here.

// Declared in natives.cpp — shared with List UI
extern std::unordered_map<ObjRef, ObjRef> g_command_listeners;

// Commands added to each Displayable (first=left soft key, rest=right soft key menu)
static std::unordered_map<ObjRef, std::vector<ObjRef>> g_commands;

// Per-Command metadata, populated from the (String, int, int) ctor.
struct CommandInfo { ObjRef label = NULL_REF; int type = 0; int priority = 0; };
static std::unordered_map<ObjRef, CommandInfo> g_command_info;

static std::unordered_map<ObjRef, SDL_Surface*> g_images;   // Image ref → surface
static std::unordered_map<ObjRef, uint32_t>     g_colors;   // Graphics ref → current color (ARGB)
static std::unordered_map<ObjRef, SDL_Surface*> g_gfx_surf; // Graphics ref → target surface
static std::unordered_map<ObjRef, SDL_Rect>     g_clips;    // Graphics ref → clip rect
static std::unordered_map<ObjRef, SDL_Point>    g_translate; // Graphics ref → translate offset
static std::unordered_map<ObjRef, ObjRef>       g_thread_runnable; // Thread ref → Runnable ref
static std::unordered_map<ObjRef, int>          g_font_size;  // Font ref → MIDP size constant
static std::unordered_map<ObjRef, int>          g_font_style; // Font ref → MIDP style constant
static std::unordered_map<ObjRef, int>          g_font_face;  // Font ref → MIDP face constant
static std::unordered_map<ObjRef, ObjRef>       g_gfx_font;   // Graphics ref → current Font ref

// Displayable currently shown (set via Display.setCurrent). Used to deliver
// input events to games that don't paint every frame (so do_repaint's key
// pump doesn't fire often enough).
ObjRef g_current_displayable = NULL_REF;

// ─── Font backend ────────────────────────────────────────────────────────────
// MIDP font size constants: SIZE_SMALL=8, SIZE_MEDIUM=0, SIZE_LARGE=16
// MIDP font style constants: STYLE_PLAIN=0, STYLE_BOLD=1, STYLE_ITALIC=2

static bool g_ttf_inited = false;

// Cache of opened TTF_Font* keyed by pixel height
static std::unordered_map<int, TTF_Font*> g_ttf_cache;
static std::unordered_map<int, TTF_Font*> g_ttf_bold_cache;

#ifdef __EMSCRIPTEN__
// Paths resolve inside the MEMFS bundle populated by the --preload-file
// options in runtime/CMakeLists.txt. CJK is omitted in the POC build.
static constexpr const char* FONT_PATH       = "/fonts/DejaVuSans.ttf";
static constexpr const char* FONT_BOLD_PATH   = "/fonts/DejaVuSans-Bold.ttf";
static constexpr const char* FONT_MONO_PATH   = "/fonts/DejaVuSansMono.ttf";
static constexpr const char* FONT_MONO_BOLD_PATH = "/fonts/DejaVuSansMono-Bold.ttf";
static constexpr const char* FONT_CJK_PATH   = "/fonts/DejaVuSans.ttf";
#elif defined(__ANDROID__)
// Resolved at first use from J2ME_FONT_DIR (set by android_platform.cpp
// after extracting bundled fonts to internal storage). std::string so the
// backing storage outlives every TTF_OpenFont call, const char* wrappers
// so the call sites stay unchanged.
static const std::string& android_font(const char* name) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    const char* dir = std::getenv("J2ME_FONT_DIR");
    std::string path = (dir ? std::string(dir) + "/" : std::string())
                     + name;
    return cache.emplace(name, std::move(path)).first->second;
}
#define FONT_PATH           (android_font("DejaVuSans.ttf").c_str())
#define FONT_BOLD_PATH      (android_font("DejaVuSans-Bold.ttf").c_str())
#define FONT_MONO_PATH      (android_font("DejaVuSansMono.ttf").c_str())
#define FONT_MONO_BOLD_PATH (android_font("DejaVuSansMono-Bold.ttf").c_str())
#define FONT_CJK_PATH       (android_font("DejaVuSans.ttf").c_str())
#else
static constexpr const char* FONT_PATH       = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
static constexpr const char* FONT_BOLD_PATH   = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
static constexpr const char* FONT_MONO_PATH   = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
static constexpr const char* FONT_MONO_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
static constexpr const char* FONT_CJK_PATH   = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
#endif

// CJK font cache (uses TCC index 0 for JP which covers most common CJK)
static std::unordered_map<int, TTF_Font*> g_ttf_cjk_cache;

static bool has_cjk(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size(); ) {
        uint8_t c = utf8[i];
        if (c < 0x80) { i += 1; continue; }
        if (c >= 0xE0) {
            // 3-byte char: CJK ranges 0x3000-0x9FFF cover Chinese/Japanese/Korean
            if (i + 2 < utf8.size()) {
                uint16_t cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
                if (cp >= 0x2E80 && cp <= 0x9FFF) return true;
                if (cp >= 0xAC00 && cp <= 0xD7AF) return true;  // Hangul
                if (cp >= 0xF900 && cp <= 0xFAFF) return true;  // CJK Compatibility
                if (cp >= 0xFF00 && cp <= 0xFFEF) return true;  // Fullwidth forms
            }
            i += 3;
        } else if (c >= 0xC0) {
            i += 2;
        } else {
            i += 1;
        }
    }
    return false;
}

extern int g_screen_w, g_screen_h;
static int midp_size_to_px(int midp_size) {
    // Pick font pixel sizes based on minimum screen dimension, matching
    // freej2me-plus's PlatformFont.fontSizes table. Bucket is the smaller of
    // width and height to match their "minimum px dimension" logic.
    int min_dim = std::min(g_screen_w, g_screen_h);
    int small, medium, large;
    if (min_dim < 128)      { small = 8;  medium = 10; large = 12; }
    else if (min_dim < 176) { small = 11; medium = 13; large = 14; }
    else if (min_dim < 220) { small = 12; medium = 13; large = 15; }
    else                    { small = 13; medium = 15; large = 17; }
    switch (midp_size) {
        case 8:  return small;   // SIZE_SMALL
        case 16: return large;   // SIZE_LARGE
        default: return medium;  // SIZE_MEDIUM (0)
    }
}

TTF_Font* get_ttf_font(int px_size, bool bold, bool mono) {
    if (!g_ttf_inited) { TTF_Init(); g_ttf_inited = true; }
    // Cache key: px_size with a bit for bold + mono combination
    int key = px_size * 4 + (bold ? 1 : 0) + (mono ? 2 : 0);
    auto& cache = g_ttf_cache;  // unified cache using combined key
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const char* path;
    if (mono) path = bold ? FONT_MONO_BOLD_PATH : FONT_MONO_PATH;
    else      path = bold ? FONT_BOLD_PATH      : FONT_PATH;
    TTF_Font* f = TTF_OpenFont(path, px_size);
    if (!f) f = TTF_OpenFont(FONT_PATH, px_size);  // fallback
    cache[key] = f;
    return f;
}

// Font Awesome Free Solid — vendored in runtime/third_party/fontawesome.
// Used by the on-screen keypad overlay for crisp icon glyphs at any size.
// Native build reads from the source path directly; emscripten build gets
// it preloaded into MEMFS at /fonts/fa-solid-900.ttf (see CMakeLists).
#ifdef __EMSCRIPTEN__
static constexpr const char* FONT_ICON_PATH = "/fonts/fa-solid-900.ttf";
#elif defined(__ANDROID__)
#define FONT_ICON_PATH (android_font("fa-solid-900.ttf").c_str())
#else
static constexpr const char* FONT_ICON_PATH =
    "third_party/fontawesome/fa-solid-900.ttf";
#endif

static std::unordered_map<int, TTF_Font*> g_ttf_icon_cache;

TTF_Font* get_icon_font(int px_size) {
    if (!g_ttf_inited) { TTF_Init(); g_ttf_inited = true; }
    auto it = g_ttf_icon_cache.find(px_size);
    if (it != g_ttf_icon_cache.end()) return it->second;
    // Try a few paths so the binary works from repo root, build dir, or
    // a user's install. First hit wins.
    const char* candidates[] = {
        FONT_ICON_PATH,
        "../third_party/fontawesome/fa-solid-900.ttf",
        "runtime/third_party/fontawesome/fa-solid-900.ttf",
    };
    TTF_Font* f = nullptr;
    const char* used = nullptr;
    for (const char* p : candidates) {
        f = TTF_OpenFont(p, px_size);
        if (f) { used = p; break; }
    }
    static bool logged_once = false;
    if (f && !logged_once) {
        fprintf(stderr, "[icon] loaded icon font: %s @ %dpx\n", used, px_size);
        logged_once = true;
    }
    if (!f)
        fprintf(stderr, "[icon] could not open Font Awesome (%s)\n", TTF_GetError());
    g_ttf_icon_cache[px_size] = f;
    return f;
}

static TTF_Font* get_cjk_font(int px_size) {
    if (!g_ttf_inited) { TTF_Init(); g_ttf_inited = true; }
    auto it = g_ttf_cjk_cache.find(px_size);
    if (it != g_ttf_cjk_cache.end()) return it->second;
    TTF_Font* f = TTF_OpenFontIndex(FONT_CJK_PATH, px_size, 0);
    g_ttf_cjk_cache[px_size] = f;
    return f;
}

static TTF_Font* font_for_obj(ObjRef font_ref) {
    int midp_size = 0;  // SIZE_MEDIUM
    int midp_style = 0; // STYLE_PLAIN
    int midp_face = 0;  // FACE_SYSTEM
    auto si = g_font_size.find(font_ref);
    if (si != g_font_size.end()) midp_size = si->second;
    auto st = g_font_style.find(font_ref);
    if (st != g_font_style.end()) midp_style = st->second;
    auto fc = g_font_face.find(font_ref);
    if (fc != g_font_face.end()) midp_face = fc->second;
    bool mono = (midp_face == 32);  // FACE_MONOSPACE
    return get_ttf_font(midp_size_to_px(midp_size), (midp_style & 1) != 0, mono);
}

static TTF_Font* font_for_gfx(ObjRef gfx_ref) {
    auto it = g_gfx_font.find(gfx_ref);
    if (it != g_gfx_font.end()) return font_for_obj(it->second);
    return get_ttf_font(midp_size_to_px(0), false, false); // default medium plain
}

// ─── Glyph cache ─────────────────────────────────────────────────────────────
// TTF_RenderUTF8_Blended allocates+frees a surface per drawString — hot path on
// sprite/text heavy frames (Bejeweled score updates, AoE text etc). Cache per-
// glyph surfaces keyed by (font*, codepoint, color); rebuild the full string
// render as a sequence of cached glyph blits.

struct GlyphKey {
    TTF_Font* font;
    uint32_t  cp;
    uint32_t  argb;
    bool operator==(const GlyphKey& o) const noexcept {
        return font == o.font && cp == o.cp && argb == o.argb;
    }
};
struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept {
        size_t h = reinterpret_cast<uintptr_t>(k.font);
        h ^= (size_t)k.cp   * 0x9E3779B97F4A7C15ULL;
        h ^= (size_t)k.argb * 0xC2B2AE3D27D4EB4FULL;
        return h;
    }
};

struct CachedGlyph {
    SDL_Surface* surf;   // nullable (e.g. missing glyph)
    int          w;
    int          h;
    int          advance;
};

static std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> g_glyph_cache;

static CachedGlyph* get_glyph(TTF_Font* font, uint32_t cp, uint32_t argb,
                              SDL_Color color) {
    GlyphKey key{font, cp, argb};
    auto it = g_glyph_cache.find(key);
    if (it != g_glyph_cache.end()) return &it->second;

    CachedGlyph g{nullptr, 0, 0, 0};
    if (font) {
        int adv = 0;
        TTF_GlyphMetrics(font, (uint16_t)cp, nullptr, nullptr, nullptr, nullptr, &adv);
        g.advance = adv;
        SDL_Surface* s = TTF_RenderGlyph_Blended(font, (uint16_t)cp, color);
        if (s) {
            // Convert to display format once, owned by cache.
            SDL_Surface* opt = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
            SDL_FreeSurface(s);
            if (opt) {
                SDL_SetSurfaceBlendMode(opt, SDL_BLENDMODE_BLEND);
                g.surf = opt;
                g.w = opt->w;
                g.h = opt->h;
            }
        }
    }
    auto [ins, _] = g_glyph_cache.emplace(key, g);
    return &ins->second;
}

// Draw `text` at (x, y) using `font` and color `argb`, honouring MIDP anchor bits.
// Returns the total x-advance drawn (not including descent).
static void draw_text_cached(SDL_Surface* target, TTF_Font* font,
                             const std::string& text,
                             int x, int y, int anchor,
                             uint32_t argb, SDL_Rect* clip, SDL_Point tx) {
    if (!target || !font || text.empty()) return;
    SDL_Color color{
        (uint8_t)((argb >> 16) & 0xFF),
        (uint8_t)((argb >> 8)  & 0xFF),
        (uint8_t)( argb        & 0xFF),
        255
    };

    // First pass: compute total width and max height for anchor correction.
    int total_w = 0, total_h = TTF_FontHeight(font);
    std::vector<CachedGlyph*> glyphs;
    glyphs.reserve(text.size());
    for (size_t off = 0; off < text.size(); ) {
        uint16_t cp;
        uint8_t c = (uint8_t)text[off];
        if (c < 0x80)          { cp = c;                                                    off += 1; }
        else if (c < 0xE0)     { cp = ((c & 0x1F) << 6) | (text[off+1] & 0x3F);             off += 2; }
        else if (c < 0xF0)     { cp = ((c & 0x0F) << 12) | ((text[off+1] & 0x3F) << 6) |
                                      (text[off+2] & 0x3F);                                 off += 3; }
        else                   { cp = 0xFFFD;                                               off += 4; }

        CachedGlyph* g = get_glyph(font, cp, argb, color);
        glyphs.push_back(g);
        total_w += g->advance;
    }

    // Apply anchor bits (HCENTER=1, RIGHT=8, BOTTOM=32, BASELINE=64)
    int draw_x = x, draw_y = y;
    if (anchor & 1)       draw_x -= total_w / 2;
    else if (anchor & 8)  draw_x -= total_w;
    if (anchor & 32)      draw_y -= total_h;
    else if (anchor & 64) draw_y -= TTF_FontAscent(font);

    draw_x += tx.x;
    draw_y += tx.y;

    for (CachedGlyph* g : glyphs) {
        if (g->surf) {
            SDL_Rect dst{draw_x, draw_y, g->w, g->h};
            SDL_BlitSurface(g->surf, nullptr, target, &dst);
        }
        draw_x += g->advance;
    }
    (void)clip;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static uint32_t j2me_color(int rgb) {
    // J2ME color is 0xRRGGBB; SDL ARGB8888 is 0xAARRGGBB
    return 0xFF000000u | static_cast<uint32_t>(rgb & 0x00FFFFFF);
}

static SDL_Surface* gfx_surface(ObjRef gfx_ref) {
    auto it = g_gfx_surf.find(gfx_ref);
    if (it != g_gfx_surf.end()) return it->second;
    return Display::instance().screen();  // default: main screen
}

static uint32_t gfx_color(ObjRef gfx_ref) {
    auto it = g_colors.find(gfx_ref);
    return it != g_colors.end() ? it->second : 0xFF000000u;
}

static SDL_Point gfx_tx(ObjRef gfx_ref) {
    auto it = g_translate.find(gfx_ref);
    return it != g_translate.end() ? it->second : SDL_Point{0, 0};
}

// Map SDL_Surface ARGB pixel to the surface's pixel format
static uint32_t map_color(SDL_Surface* surf, uint32_t argb) {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b =  argb        & 0xFF;
    return SDL_MapRGBA(surf->format, r, g, b, a);
}

// Fill a rectangle on a surface with the current color
static void fill_rect_on(SDL_Surface* surf, uint32_t color,
                          int x, int y, int w, int h) {
    if (!surf || w <= 0 || h <= 0) return;
    SDL_Rect r{x, y, w, h};
    SDL_FillRect(surf, &r, map_color(surf, color));
}

// Draw a horizontal line
static void draw_hline(SDL_Surface* surf, uint32_t color, int x, int y, int w) {
    fill_rect_on(surf, color, x, y, w, 1);
}

// Draw a vertical line
static void draw_vline(SDL_Surface* surf, uint32_t color, int x, int y, int h) {
    fill_rect_on(surf, color, x, y, 1, h);
}

// Scanline triangle fill. Sort vertices by y, then for each row compute the
// two edges' x-intersections and fill that span with a single hline.
static void fill_triangle_on(SDL_Surface* surf, uint32_t color,
                             int x1, int y1, int x2, int y2, int x3, int y3) {
    if (!surf) return;
    // Sort so y1 <= y2 <= y3
    if (y2 < y1) { std::swap(y1, y2); std::swap(x1, x2); }
    if (y3 < y1) { std::swap(y1, y3); std::swap(x1, x3); }
    if (y3 < y2) { std::swap(y2, y3); std::swap(x2, x3); }
    if (y1 == y3) {  // Degenerate: flat line
        int xa = std::min({x1, x2, x3});
        int xb = std::max({x1, x2, x3});
        draw_hline(surf, color, xa, y1, xb - xa + 1);
        return;
    }
    // For each scanline y in [y1..y3], compute x on the long edge (v1→v3) and
    // x on the active short edge (v1→v2 for upper half, v2→v3 for lower).
    auto interp = [](int y, int ya, int xa, int yb, int xb) -> int {
        if (yb == ya) return xa;
        return xa + (xb - xa) * (y - ya) / (yb - ya);
    };
    for (int y = y1; y <= y3; ++y) {
        int xl = interp(y, y1, x1, y3, x3);
        int xr = (y < y2) ? interp(y, y1, x1, y2, x2)
                          : interp(y, y2, x2, y3, x3);
        if (xr < xl) std::swap(xl, xr);
        draw_hline(surf, color, xl, y, xr - xl + 1);
    }
}

// Is the given point's angle (in degrees, 0°=3-o'clock, CCW) inside the arc?
static inline bool arc_includes(int dx, int dy, int rx, int ry,
                                int startAngle, int arcAngle) {
    if (arcAngle >= 360 || arcAngle <= -360) return true;
    double a = std::atan2(-(double)dy * rx, (double)dx * ry) * 180.0 / M_PI;
    if (a < 0) a += 360;
    double s = startAngle, e = startAngle + arcAngle;
    if (arcAngle < 0) { s = startAngle + arcAngle; e = startAngle; }
    while (s < 0)     { s += 360; e += 360; }
    while (s >= 360)  { s -= 360; e -= 360; }
    if (e <= 360)     return a >= s && a <= e;
    return a >= s || a <= (e - 360);
}

static void fill_arc_on(SDL_Surface* surf, uint32_t color,
                        int x, int y, int w, int h,
                        int startAngle, int arcAngle) {
    if (!surf || w <= 0 || h <= 0) return;
    int rx = w / 2, ry = h / 2;
    int cx = x + rx, cy = y + ry;
    uint32_t mapped = map_color(surf, color);
    SDL_Rect bbox{x, y, w, h};
    SDL_Rect isect;
    SDL_Rect clip; SDL_GetClipRect(surf, &clip);
    if (!SDL_IntersectRect(&bbox, &clip, &isect)) return;
    for (int py = isect.y; py < isect.y + isect.h; ++py) {
        int dy = py - cy;
        for (int px = isect.x; px < isect.x + isect.w; ++px) {
            int dx = px - cx;
            double nx = (double)dx / rx, ny = (double)dy / ry;
            if (nx*nx + ny*ny > 1.0) continue;
            if (!arc_includes(dx, dy, rx, ry, startAngle, arcAngle)) continue;
            SDL_Rect one{px, py, 1, 1};
            SDL_FillRect(surf, &one, mapped);
        }
    }
}

static void draw_arc_on(SDL_Surface* surf, uint32_t color,
                        int x, int y, int w, int h,
                        int startAngle, int arcAngle) {
    if (!surf || w <= 0 || h <= 0) return;
    int rx = w / 2, ry = h / 2;
    int cx = x + rx, cy = y + ry;
    uint32_t mapped = map_color(surf, color);
    // SDL_FillRect already honours the clip rect, so the 1×1 fills below
    // are automatically clipped — but do an early-out on an empty clip.
    SDL_Rect clip; SDL_GetClipRect(surf, &clip);
    if (clip.w <= 0 || clip.h <= 0) return;
    double step = 1.0 / std::max(rx, ry);
    double a0 = startAngle * M_PI / 180.0;
    double a1 = (startAngle + arcAngle) * M_PI / 180.0;
    if (arcAngle < 0) std::swap(a0, a1);
    for (double a = a0; a <= a1; a += step) {
        int px = cx + (int)std::round(rx * std::cos(a));
        int py = cy - (int)std::round(ry * std::sin(a));
        SDL_Rect one{px, py, 1, 1};
        SDL_FillRect(surf, &one, mapped);
    }
}

// Bresenham line. Clips against the Graphics clip rect, not just the
// surface bounds — several games (Age of Empires II Mobile's tile-
// highlight outline) rely on setClip to keep drawLine inside a sub-
// rectangle. Without this, lines leak over the HUD.
static void draw_line_on(SDL_Surface* surf, uint32_t color,
                          int x1, int y1, int x2, int y2) {
    if (!surf) return;
    uint32_t mc = map_color(surf, color);

    SDL_Rect clip; SDL_GetClipRect(surf, &clip);
    // An empty clip means "draw nothing" per MIDP semantics.
    if (clip.w <= 0 || clip.h <= 0) return;
    int cx0 = clip.x, cy0 = clip.y;
    int cx1 = clip.x + clip.w, cy1 = clip.y + clip.h;

    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (x1 >= cx0 && x1 < cx1 && y1 >= cy0 && y1 < cy1) {
            uint32_t* p = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(surf->pixels) + y1 * surf->pitch + x1 * 4);
            *p = mc;
        }
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

// Blit an image surface onto a target surface
static void blit_image(SDL_Surface* dst, SDL_Surface* src,
                        int dx, int dy,
                        int sx, int sy, int sw, int sh,
                        int transform) {
    if (!dst || !src) return;

    // MIDP transform constants (from javax.microedition.lcdui.game.Sprite):
    //   TRANS_NONE          = 0
    //   TRANS_MIRROR_ROT180 = 1  (= vertical flip)
    //   TRANS_MIRROR        = 2  (horizontal flip)
    //   TRANS_ROT180        = 3
    //   TRANS_MIRROR_ROT270 = 4
    //   TRANS_ROT90         = 5
    //   TRANS_ROT270        = 6
    //   TRANS_MIRROR_ROT90  = 7
    if (transform == 0) {
        SDL_Rect srect{sx, sy, sw, sh};
        SDL_Rect drect{dx, dy, sw, sh};
        SDL_BlitSurface(src, &srect, dst, &drect);
    } else {
        // Rotations (90/270) swap w/h
        int dw = sw, dh = sh;
        if (transform == 4 || transform == 5 || transform == 6 || transform == 7) {
            dw = sh; dh = sw;
        }

        // Fast source-pixel access. All MIDP Image surfaces pass through
        // SDL_ConvertSurfaceFormat(ARGB8888) in load_png_from_bytes, so the
        // common case is a direct read from src->pixels at (sx,sy,sw,sh).
        // Only fall back to an intermediate region copy for exotic formats
        // (e.g. palette surfaces from non-PNG paths). This saves a malloc +
        // SDL_BlitSurface + free on every transformed blit.
        SDL_Surface* region = nullptr;
        const uint8_t* sp;
        int src_pitch;
        if (src->format->format == SDL_PIXELFORMAT_ARGB8888) {
            SDL_LockSurface(src);
            sp        = (const uint8_t*)src->pixels + sy * src->pitch + sx * 4;
            src_pitch = src->pitch;
        } else {
            region = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!region) return;
            SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
            SDL_Rect srect{sx, sy, sw, sh};
            SDL_BlitSurface(src, &srect, region, nullptr);
            SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_BLEND);
            SDL_LockSurface(region);
            sp        = (const uint8_t*)region->pixels;
            src_pitch = region->pitch;
        }

        // Dest-sized surface that receives the rotated pixels.
        SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!tmp) {
            if (region) { SDL_UnlockSurface(region); SDL_FreeSurface(region); }
            else        { SDL_UnlockSurface(src); }
            return;
        }
        SDL_FillRect(tmp, nullptr, 0x00000000u);

        SDL_LockSurface(tmp);
        // Hoist the transform switch out of the inner loop — the original
        // pixel-by-pixel switch+bounds-check hit 7-10% of CPU on games that
        // rotate sprites every frame (Doom RPG / AoE). Each branch below is
        // a specialised tight loop whose `dr`/`dc` formulas the compiler can
        // strength-reduce. `tmp` was FillRect'd to 0 above, so skipping the
        // alpha-0 write just saves a store — dropping that check lets the
        // copy become a pure sequential access pattern.
        uint8_t*  dp = (uint8_t*)tmp->pixels;
        const int dst_pitch = tmp->pitch;
        // Each transform is a permutation of the dw×dh grid onto the sw×sh
        // grid (with dw/dh swapped for 90/270° rotations), so (dr,dc) is
        // always in-bounds — the historical `if (dc<0 || dc>=dw || …)`
        // was dead code. Dropping it and inlining the pixel store turns
        // the inner loop into a straight load/compare/store sequence.
        #define TRANSFORM_LOOP(DR, DC)                                              \
            for (int r = 0; r < sh; ++r) {                                          \
                const uint8_t* row = sp + r * src_pitch;                            \
                for (int c = 0; c < sw; ++c) {                                      \
                    uint32_t px;                                                    \
                    std::memcpy(&px, row + c * 4, 4);                               \
                    if ((px >> 24) == 0) continue;                                  \
                    std::memcpy(dp + (DR) * dst_pitch + (DC) * 4, &px, 4);          \
                }                                                                   \
            }
        switch (transform) {
            case 1: TRANSFORM_LOOP(sh-1-r, c);      break;  // MIRROR_ROT180 (vertical flip)
            case 2: TRANSFORM_LOOP(r,      sw-1-c); break;  // MIRROR (horizontal flip)
            case 3: TRANSFORM_LOOP(sh-1-r, sw-1-c); break;  // ROT180
            case 4: TRANSFORM_LOOP(sw-1-c, sh-1-r); break;  // MIRROR_ROT270
            case 5: TRANSFORM_LOOP(c,      sh-1-r); break;  // ROT90 clockwise
            case 6: TRANSFORM_LOOP(sw-1-c, r);      break;  // ROT270 counter-clockwise
            case 7: TRANSFORM_LOOP(c,      r);      break;  // MIRROR_ROT90
            default: TRANSFORM_LOOP(r,     c);      break;  // shouldn't occur (TRANS_NONE handled above)
        }
        #undef TRANSFORM_LOOP
        SDL_UnlockSurface(tmp);
        if (region) { SDL_UnlockSurface(region); SDL_FreeSurface(region); }
        else        { SDL_UnlockSurface(src); }
        SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);
        SDL_Rect drect{dx, dy, dw, dh};
        SDL_BlitSurface(tmp, nullptr, dst, &drect);
        SDL_FreeSurface(tmp);
    }
}

// ─── Image loading ────────────────────────────────────────────────────────────

// JAMDAT-era games (Bejeweled 2007, Tetris Pop, etc.) pack PNGs in a custom
// format that libpng rejects as "invalid chunk type". The format:
//   - Standard 8-byte PNG magic
//   - 4-byte JAMDAT header {0x00, 0xC0, 0x00, 0x80} (purpose unclear, possibly
//     an encoder version marker — appears unchanged across titles)
//   - IHDR in compact form: 2-byte length + 4-byte type + 13 data bytes +
//     2-byte truncated CRC (only the high 2 bytes of the real CRC)
//   - Subsequent chunks (PLTE, tRNS, IDAT, …) in STANDARD PNG format, but
//     with a variable trailer — either 4 bytes (standard CRC) or 6 bytes
//     (CRC + 2 bytes alignment padding, seen between IDAT→IEND)
//   - No trailing IEND chunk; file ends with "JAMDAT header + IEND type"
//
// Conversion to standard PNG: rebuild IHDR, copy downstream chunks verbatim
// (probing trailer size via CRC match), append a fresh IEND. The result is
// byte-identical to what a standards-compliant encoder would emit.
static bool looks_like_jamdat_png(const uint8_t* data, size_t len) {
    static const uint8_t kMagic[12] = {
        0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0x00,0xC0,0x00,0x80 };
    return len >= 32 && std::memcmp(data, kMagic, 12) == 0;
}
static std::vector<uint8_t> repair_jamdat_png(const uint8_t* in, size_t n) {
    auto push_u32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
        v.push_back((x >>  8) & 0xFF); v.push_back( x        & 0xFF);
    };
    auto push_chunk = [&](std::vector<uint8_t>& v, const char* type,
                          const uint8_t* data, size_t dlen) {
        push_u32(v, (uint32_t)dlen);
        const uint8_t* t = (const uint8_t*)type;
        v.insert(v.end(), t, t + 4);
        v.insert(v.end(), data, data + dlen);
        uLong c = crc32(0, t, 4);
        c = crc32(c, data, (uInt)dlen);
        push_u32(v, (uint32_t)c);
    };

    std::vector<uint8_t> out;
    out.reserve(n + 8);
    out.insert(out.end(), in, in + 8);            // PNG magic
    if (n < 0x1F) return {};
    push_chunk(out, "IHDR", in + 0x12, 13);       // rebuild IHDR from its 13 data bytes

    // Find and copy subsequent chunks by scanning type signatures.
    static const char* kChunkTypes[] = {
        "PLTE", "tRNS", "IDAT", "gAMA", "cHRM", "bKGD",
        "pHYs", "sRGB", "iCCP", "tEXt", "zTXt",
    };
    struct Hit { size_t pos; const char* type; };
    std::vector<Hit> hits;
    for (const char* t : kChunkTypes) {
        for (size_t i = 0x21; i + 4 <= n; ++i) {
            if (std::memcmp(in + i, t, 4) == 0) hits.push_back({i, t});
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pos < b.pos; });

    for (size_t k = 0; k < hits.size(); ++k) {
        size_t data_start = hits[k].pos + 4;
        size_t next_pos   = (k + 1 < hits.size()) ? hits[k+1].pos : n;
        // Try trailer sizes 4 (standard CRC) and 6 (CRC + 2 padding). Pick
        // whichever yields a CRC that matches the stored value.
        size_t chosen_len = 0; bool ok = false;
        for (int trailer : {4, 6}) {
            long long length = (long long)next_pos - trailer - 4 - (long long)data_start;
            if (length < 0 || (size_t)data_start + length + 4 > n) continue;
            uint32_t stored = ((uint32_t)in[data_start + length]     << 24) |
                              ((uint32_t)in[data_start + length + 1] << 16) |
                              ((uint32_t)in[data_start + length + 2] <<  8) |
                              ((uint32_t)in[data_start + length + 3]);
            uLong c = crc32(0, (const uint8_t*)hits[k].type, 4);
            c = crc32(c, in + data_start, (uInt)length);
            if (stored == (uint32_t)c) { chosen_len = (size_t)length; ok = true; break; }
        }
        if (!ok) {
            // CRC doesn't verify either way — fall back to next_pos - 8.
            long long length = (long long)next_pos - 8 - (long long)data_start;
            if (length < 0) length = 0;
            chosen_len = (size_t)length;
        }
        push_chunk(out, hits[k].type, in + data_start, chosen_len);
    }
    push_chunk(out, "IEND", nullptr, 0);
    return out;
}

// Load PNG bytes → SDL_Surface (ARGB8888)
static SDL_Surface* load_png_from_bytes(const uint8_t* data, size_t len) {
    // JAMDAT's custom PNG format (Bejeweled / Tetris Pop / etc.) fails libpng's
    // chunk validator. Repair it to standard PNG before decoding. Detection is
    // exact — harmless false positives are impossible because the signature is
    // an invalid standard-PNG chunk length anyway.
    std::vector<uint8_t> repaired;
    if (looks_like_jamdat_png(data, len)) {
        repaired = repair_jamdat_png(data, len);
        if (!repaired.empty()) { data = repaired.data(); len = repaired.size(); }
    }

    SDL_RWops* rw = SDL_RWFromConstMem(data, static_cast<int>(len));
    if (!rw) return nullptr;
    SDL_Surface* raw = IMG_Load_RW(rw, 1);  // 1 = auto-close
    if (!raw) return nullptr;
    // Preserve color key transparency through format conversion
    uint32_t ckey = 0;
    bool has_colorkey = (SDL_GetColorKey(raw, &ckey) == 0);
    bool has_alpha = (raw->format->Amask != 0);

    // For indexed PNGs with a color key, build a per-pixel transparency
    // mask from the SOURCE palette indices (not from post-convert RGB).
    // A common Doom RPG / Bejeweled 3 pattern is a palette that has both
    // a transparent-white entry (color-key index, alpha 0) and an opaque-
    // white entry for the sprite body — both map to RGB (255,255,255) in
    // ARGB8888, so matching on RGB eats the visible white pixels too.
    std::vector<bool> transparent_mask;
    bool indexed_ckey = has_colorkey && raw->format->BytesPerPixel == 1;
    if (indexed_ckey) {
        SDL_LockSurface(raw);
        const uint8_t* src = static_cast<const uint8_t*>(raw->pixels);
        int pitch = raw->pitch;
        transparent_mask.resize((size_t)raw->w * raw->h, false);
        uint8_t ck_idx = (uint8_t)(ckey & 0xFF);
        for (int y = 0; y < raw->h; ++y)
            for (int x = 0; x < raw->w; ++x)
                if (src[y * pitch + x] == ck_idx)
                    transparent_mask[(size_t)y * raw->w + x] = true;
        SDL_UnlockSurface(raw);
    }
    // Non-indexed color-key fallback: record the RGB value; we'll match on
    // it after conversion the old way.
    uint8_t ck_r = 0, ck_g = 0, ck_b = 0;
    if (has_colorkey && !indexed_ckey)
        SDL_GetRGB(ckey, raw->format, &ck_r, &ck_g, &ck_b);

    SDL_Surface* converted = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!converted) return nullptr;

    SDL_LockSurface(converted);
    uint32_t* px = static_cast<uint32_t*>(converted->pixels);
    int count = converted->w * converted->h;
    if (indexed_ckey) {
        for (int i = 0; i < count; i++) {
            if (transparent_mask[(size_t)i])
                px[i] = 0x00000000u;
            else
                px[i] |= 0xFF000000u;
        }
    } else if (has_colorkey) {
        uint32_t ck_rgb = ((uint32_t)ck_r << 16) | ((uint32_t)ck_g << 8) | ck_b;
        for (int i = 0; i < count; i++) {
            if ((px[i] & 0x00FFFFFFu) == ck_rgb)
                px[i] = 0x00000000u;
            else
                px[i] |= 0xFF000000u;
        }
    } else if (!has_alpha) {
        for (int i = 0; i < count; i++)
            px[i] |= 0xFF000000u;
    }
    SDL_UnlockSurface(converted);
    SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_BLEND);
    return converted;
}

// ─── Registration ─────────────────────────────────────────────────────────────

int  g_screen_w = 240;
int  g_screen_h = 320;
bool g_screen_explicit = false;  // user passed WxH on command line
bool g_auto_res = false;         // user passed --auto-res

// ─── Auto-detect screen resolution ───────────────────────────────────────────
// J2ME has no standard manifest field for target screen size, but several
// signals work in practice:
//   1. boxal.inf key 21,W,H (BoxAL framework — Connect2Media titles)
//   2. Vendor-specific manifest extensions (Nokia/Samsung/SE on some titles)
//   3. PNG modal dimension — many games include a splash/background PNG
//      whose dimensions match the target Canvas
// We only override the default 240x320 if the signal is unambiguous.
static void try_detect_screen_resolution(const JarFile& jar) {
    if (g_screen_explicit) {
        fprintf(stderr, "[display] resolution: %dx%d (explicit)\n",
                g_screen_w, g_screen_h);
        return;
    }
    // boxal.inf is BoxAL-specific and the game won't render at all without
    // the right size, so always honor it. Other signals (manifest extensions,
    // PNG mode) only run with --auto-res because they sometimes pick the
    // wrong size and break visual layout — too coarse to be the default.

    // Signal 1: boxal.inf
    if (jar.has("boxal.inf")) {
        auto& data = jar.get("boxal.inf");
        std::string inf(data.begin(), data.end());
        size_t pos = 0;
        while (pos < inf.size()) {
            size_t eol = inf.find_first_of("\r\n", pos);
            if (eol == std::string::npos) eol = inf.size();
            std::string line = inf.substr(pos, eol - pos);
            if (line.substr(0, 3) == "21,") {
                int w = 0, h = 0;
                if (sscanf(line.c_str(), "21,%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                    g_screen_w = w; g_screen_h = h;
                    fprintf(stderr, "[display] resolution from boxal.inf: %dx%d\n", w, h);
                    return;
                }
            }
            pos = eol;
            while (pos < inf.size() && (inf[pos] == '\r' || inf[pos] == '\n')) ++pos;
        }
    }

    if (!g_auto_res) {
        fprintf(stderr,
            "[display] resolution: %dx%d (default; pass WxH or --auto-res to change)\n",
            g_screen_w, g_screen_h);
        return;
    }

    // Signal 2: vendor manifest extensions. None are standard, but several
    // SDKs include them. Parse the manifest once and look up the keys.
    if (jar.has("META-INF/MANIFEST.MF")) {
        const auto& data = jar.get("META-INF/MANIFEST.MF");
        std::string mf(data.begin(), data.end());
        // Same WxH parser
        auto try_parse_size = [](const std::string& v, int& w, int& h) -> bool {
            return sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
        };
        std::unordered_map<std::string, std::string> kv;
        size_t pos = 0;
        while (pos < mf.size()) {
            size_t eol = mf.find_first_of("\r\n", pos);
            if (eol == std::string::npos) eol = mf.size();
            std::string line = mf.substr(pos, eol - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                    val.erase(0, 1);
                kv[k] = val;
            }
            pos = eol;
            while (pos < mf.size() && (mf[pos] == '\r' || mf[pos] == '\n')) ++pos;
        }
        for (const char* key : {
            "Nokia-MIDlet-Original-Display-Size",
            "Nokia-MIDlet-Target-Display-Size",
            "MIDxlet-Display-Size",
            "Display-Size",
        }) {
            auto it = kv.find(key);
            int w = 0, h = 0;
            if (it != kv.end() && try_parse_size(it->second, w, h)) {
                g_screen_w = w; g_screen_h = h;
                fprintf(stderr, "[display] resolution from manifest %s: %dx%d\n",
                        key, w, h);
                return;
            }
        }
    }

    // Signal 3: PNG modal dimension. Many games include their splash /
    // background as a top-level PNG sized to the target screen. We pick the
    // mode of all non-icon PNGs (>= 96 in both dims). Only override if at
    // least 3 PNGs share the same dimension (high-confidence signal — tiny
    // sprites won't dominate, and a single large image could be a wallpaper
    // unrelated to screen size).
    std::unordered_map<uint64_t, int> dim_counts;
    int max_w = 0, max_h = 0;
    for (auto& entry : jar.entries_with_suffix(".png")) {
        const auto& data = jar.get(entry);
        if (data.size() < 24) continue;
        // PNG signature + IHDR length(4) + type "IHDR"(4) + width(4) + height(4)
        if (std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) != 0) continue;
        // Big-endian width at offset 16, height at 20
        auto be32 = [](const uint8_t* p) {
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
        };
        int w = (int)be32((uint8_t*)data.data() + 16);
        int h = (int)be32((uint8_t*)data.data() + 20);
        if (w < 96 || h < 96) continue;  // skip icons / sprites
        if (w > 1024 || h > 1024) continue; // skip oversize
        // Aspect-ratio filter: phone screens cluster between 0.5:1 and 2:1.
        // Sprite atlases (long horizontal strips) routinely break this — e.g.
        // a 479x122 frame strip is not a screen. Drop anything more elongated.
        double aspect = (double)w / h;
        if (aspect < 0.5 || aspect > 2.0) continue;
        dim_counts[((uint64_t)w << 32) | (uint32_t)h]++;
        if ((int64_t)w * h > (int64_t)max_w * max_h) { max_w = w; max_h = h; }
    }
    int best_count = 0; uint64_t best_key = 0;
    for (auto& [k, c] : dim_counts) if (c > best_count) { best_count = c; best_key = k; }
    if (best_count >= 3) {
        g_screen_w = (int)(best_key >> 32);
        g_screen_h = (int)(best_key & 0xFFFFFFFFu);
        fprintf(stderr, "[display] resolution from %d PNGs at common size: %dx%d\n",
                best_count, g_screen_w, g_screen_h);
        return;
    }

    fprintf(stderr, "[display] resolution: defaulting to %dx%d (no signal)\n",
            g_screen_w, g_screen_h);
}

void register_graphics_natives(VM& vm, const JarFile& jar) {

    try_detect_screen_resolution(jar);

    // ── Graphics ─────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Graphics",
        "setColor", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            g_colors[self] = j2me_color(args[1].as_int());
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "setColor", "(III)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            uint32_t r = static_cast<uint8_t>(args[1].as_int());
            uint32_t g = static_cast<uint8_t>(args[2].as_int());
            uint32_t b = static_cast<uint8_t>(args[3].as_int());
            g_colors[self] = 0xFF000000u | (r << 16) | (g << 8) | b;
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "getColor", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(static_cast<int32_t>(gfx_color(args[0].as_ref()) & 0x00FFFFFFu));
        });

    // Spec: closest displayable color. We render at 24-bit truecolor.
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getDisplayColor", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(args[1].as_int() & 0x00FFFFFFu);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getRedComponent", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int((gfx_color(args[0].as_ref()) >> 16) & 0xFF);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getGreenComponent", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int((gfx_color(args[0].as_ref()) >> 8) & 0xFF);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getBlueComponent", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(gfx_color(args[0].as_ref()) & 0xFF);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "setGrayScale", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            uint32_t g = (uint32_t)(args[1].as_int() & 0xFF);
            g_colors[self] = 0xFF000000u | (g << 16) | (g << 8) | g;
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getGrayScale", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            uint32_t c = gfx_color(args[0].as_ref());
            int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            f.push_int((r + g + b) / 3);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "fillRect", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            int x = args[1].as_int() + t.x, y = args[2].as_int() + t.y;
            int w = args[3].as_int(), h = args[4].as_int();
            if (std::getenv("J2ME_TRACE_DRAW"))
                fprintf(stderr, "[draw] fillRect (%d,%d) %dx%d color=0x%x\n",
                        x, y, w, h, gfx_color(self));
            fill_rect_on(gfx_surface(self), gfx_color(self), x, y, w, h);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "fillTriangle", "(IIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            fill_triangle_on(gfx_surface(self), gfx_color(self),
                             args[1].as_int() + t.x, args[2].as_int() + t.y,
                             args[3].as_int() + t.x, args[4].as_int() + t.y,
                             args[5].as_int() + t.x, args[6].as_int() + t.y);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "fillArc", "(IIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            fill_arc_on(gfx_surface(self), gfx_color(self),
                        args[1].as_int() + t.x, args[2].as_int() + t.y,
                        args[3].as_int(), args[4].as_int(),
                        args[5].as_int(), args[6].as_int());
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawArc", "(IIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            draw_arc_on(gfx_surface(self), gfx_color(self),
                        args[1].as_int() + t.x, args[2].as_int() + t.y,
                        args[3].as_int(), args[4].as_int(),
                        args[5].as_int(), args[6].as_int());
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawRect", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef  self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            uint32_t col = gfx_color(self);
            auto t = gfx_tx(self);
            int x = args[1].as_int() + t.x, y = args[2].as_int() + t.y;
            int w = args[3].as_int(), h = args[4].as_int();
            draw_hline(s, col, x,     y,     w);
            draw_hline(s, col, x,     y+h-1, w);
            draw_vline(s, col, x,     y,     h);
            draw_vline(s, col, x+w-1, y,     h);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawLine", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            int x1 = args[1].as_int() + t.x, y1 = args[2].as_int() + t.y;
            int x2 = args[3].as_int() + t.x, y2 = args[4].as_int() + t.y;
            if (std::getenv("J2ME_TRACE_DRAW"))
                fprintf(stderr, "[draw] drawLine (%d,%d)->(%d,%d) color=0x%x\n",
                        x1, y1, x2, y2, gfx_color(self));
            draw_line_on(gfx_surface(self), gfx_color(self), x1, y1, x2, y2);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "fillRoundRect", "(IIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto t = gfx_tx(self);
            fill_rect_on(gfx_surface(self), gfx_color(self),
                         args[1].as_int() + t.x, args[2].as_int() + t.y,
                         args[3].as_int(), args[4].as_int());
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawRoundRect", "(IIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            uint32_t col = gfx_color(self);
            int x = args[1].as_int(), y = args[2].as_int();
            int w = args[3].as_int(), h = args[4].as_int();
            draw_hline(s, col, x, y,     w);
            draw_hline(s, col, x, y+h-1, w);
            draw_vline(s, col, x,     y, h);
            draw_vline(s, col, x+w-1, y, h);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawString", "(Ljava/lang/String;III)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            std::string text = v.string_value(args[1].as_ref());
            if (text.empty()) return;
            int x = args[2].as_int();
            int y = args[3].as_int();
            int anchor = args[4].as_int();

            SDL_Surface* target = gfx_surface(self);
            if (!target) return;
            TTF_Font* font = font_for_gfx(self);
            if (!font) return;
            if (has_cjk(text)) {
                int px = TTF_FontHeight(font) - 2;
                if (px < 9) px = 9;
                TTF_Font* cjk = get_cjk_font(px);
                if (cjk) font = cjk;
            }

            uint32_t argb = 0xFF000000u;
            auto cit = g_colors.find(self);
            if (cit != g_colors.end()) argb = cit->second;

            draw_text_cached(target, font, text, x, y, anchor, argb,
                             nullptr, gfx_tx(self));
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawSubstring",
        "(Ljava/lang/String;IIIII)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            std::string full = v.string_value(args[1].as_ref());
            int offset = args[2].as_int();
            int len    = args[3].as_int();
            int x      = args[4].as_int();
            int y      = args[5].as_int();
            int anchor = args[6].as_int();

            if (len <= 0) return;
            std::string text = full.substr(offset, len);
            if (text.empty()) return;

            SDL_Surface* target = gfx_surface(self);
            if (!target) return;
            TTF_Font* font = font_for_gfx(self);
            if (!font) return;
            if (has_cjk(text)) {
                int px = TTF_FontHeight(font) - 2;
                if (px < 9) px = 9;
                TTF_Font* cjk = get_cjk_font(px);
                if (cjk) font = cjk;
            }

            uint32_t argb = 0xFF000000u;
            auto cit = g_colors.find(self);
            if (cit != g_colors.end()) argb = cit->second;

            draw_text_cached(target, font, text, x, y, anchor, argb,
                             nullptr, gfx_tx(self));
        });

    // Encode a UTF-16 code unit into UTF-8. Games overwhelmingly use BMP
    // characters; unpaired surrogates are rendered as U+FFFD.
    auto append_u16_utf8 = [](std::string& out, uint16_t cu) {
        if (cu < 0x80) {
            out.push_back((char)cu);
        } else if (cu < 0x800) {
            out.push_back((char)(0xC0 | (cu >> 6)));
            out.push_back((char)(0x80 | (cu & 0x3F)));
        } else if (cu >= 0xD800 && cu <= 0xDFFF) {
            out.append("\xEF\xBF\xBD"); // U+FFFD
        } else {
            out.push_back((char)(0xE0 | (cu >> 12)));
            out.push_back((char)(0x80 | ((cu >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cu & 0x3F)));
        }
    };

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawChar", "(CIII)V",
        [append_u16_utf8](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            uint16_t ch = (uint16_t)args[1].as_int();
            int x      = args[2].as_int();
            int y      = args[3].as_int();
            int anchor = args[4].as_int();

            SDL_Surface* target = gfx_surface(self);
            if (!target) return;
            TTF_Font* font = font_for_gfx(self);
            if (!font) return;

            std::string text;
            append_u16_utf8(text, ch);
            if (has_cjk(text)) {
                int px = TTF_FontHeight(font) - 2;
                if (px < 9) px = 9;
                TTF_Font* cjk = get_cjk_font(px);
                if (cjk) font = cjk;
            }

            uint32_t argb = 0xFF000000u;
            auto cit = g_colors.find(self);
            if (cit != g_colors.end()) argb = cit->second;

            draw_text_cached(target, font, text, x, y, anchor, argb,
                             nullptr, gfx_tx(self));
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawChars", "([CIIIII)V",
        [append_u16_utf8](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ObjRef arr  = args[1].as_ref();
            int offset = args[2].as_int();
            int len    = args[3].as_int();
            int x      = args[4].as_int();
            int y      = args[5].as_int();
            int anchor = args[6].as_int();

            if (len <= 0 || arr == NULL_REF) return;
            HeapObject* ao = v.heap().deref(arr);
            if (!ao) return;
            int32_t src_len = ao->array_length();
            if (offset < 0 || offset >= src_len) return;
            if (offset + len > src_len) len = src_len - offset;
            if (len <= 0) return;

            SDL_Surface* target = gfx_surface(self);
            if (!target) return;
            TTF_Font* font = font_for_gfx(self);
            if (!font) return;

            std::string text;
            uint16_t* src = ao->array_shorts();
            text.reserve((size_t)len);
            for (int i = 0; i < len; ++i)
                append_u16_utf8(text, src[offset + i]);

            if (has_cjk(text)) {
                int px = TTF_FontHeight(font) - 2;
                if (px < 9) px = 9;
                TTF_Font* cjk = get_cjk_font(px);
                if (cjk) font = cjk;
            }

            uint32_t argb = 0xFF000000u;
            auto cit = g_colors.find(self);
            if (cit != g_colors.end()) argb = cit->second;

            draw_text_cached(target, font, text, x, y, anchor, argb,
                             nullptr, gfx_tx(self));
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "setClip", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            if (!s) return;
            auto t = gfx_tx(self);
            SDL_Rect clip{args[1].as_int() + t.x, args[2].as_int() + t.y,
                          args[3].as_int(), args[4].as_int()};
            g_clips[self] = clip;
            SDL_SetClipRect(s, &clip);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "clipRect", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            if (!s) return;
            auto t = gfx_tx(self);
            SDL_Rect nr{args[1].as_int() + t.x, args[2].as_int() + t.y,
                        args[3].as_int(), args[4].as_int()};
            SDL_Rect cur;
            SDL_GetClipRect(s, &cur);
            SDL_Rect isect;
            if (SDL_IntersectRect(&cur, &nr, &isect)) {
                SDL_SetClipRect(s, &isect);
            } else {
                // MIDP spec: empty intersection clips out all subsequent draws.
                SDL_Rect empty{0, 0, 0, 0};
                SDL_SetClipRect(s, &empty);
            }
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "getClipX", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            auto t = gfx_tx(self);
            SDL_Rect r; SDL_GetClipRect(s, &r);
            f.push_int(r.x - t.x);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getClipY", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            auto t = gfx_tx(self);
            SDL_Rect r; SDL_GetClipRect(s, &r);
            f.push_int(r.y - t.y);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getClipWidth", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            SDL_Surface* s = gfx_surface(args[0].as_ref());
            SDL_Rect r; SDL_GetClipRect(s, &r); f.push_int(r.w);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getClipHeight", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            SDL_Surface* s = gfx_surface(args[0].as_ref());
            SDL_Rect r; SDL_GetClipRect(s, &r); f.push_int(r.h);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "translate", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            auto& t = g_translate[self];
            t.x += args[1].as_int();
            t.y += args[2].as_int();
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "getTranslateX", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(gfx_tx(args[0].as_ref()).x);
        });
    vm.register_native("javax/microedition/lcdui/Graphics",
        "getTranslateY", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(gfx_tx(args[0].as_ref()).y);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawImage",
        "(Ljavax/microedition/lcdui/Image;III)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef gfx = args[0].as_ref(), img = args[1].as_ref();
            int x = args[2].as_int(), y = args[3].as_int();
            int anchor = args[4].as_int();
            auto it = g_images.find(img);
            if (it == g_images.end()) return;
            SDL_Surface* src = it->second;
            SDL_Surface* dst = gfx_surface(gfx);
            auto t = gfx_tx(gfx);
            // MIDP anchors: HCENTER=1, VCENTER=2, RIGHT=8, BOTTOM=32, BASELINE=64
            if (anchor & 1)  x -= src->w / 2;
            if (anchor & 8)  x -= src->w;
            if (anchor & 2)  y -= src->h / 2;
            if (anchor & 32) y -= src->h;
            SDL_Rect drect{x + t.x, y + t.y, src->w, src->h};
            SDL_BlitSurface(src, nullptr, dst, &drect);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawRegion",
        "(Ljavax/microedition/lcdui/Image;IIIIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef gfx = args[0].as_ref(), img = args[1].as_ref();
            int sx = args[2].as_int(), sy = args[3].as_int();
            int sw = args[4].as_int(), sh = args[5].as_int();
            int  t = args[6].as_int();   // transform
            int dx = args[7].as_int(), dy = args[8].as_int();
            int anchor = args[9].as_int();
            auto it = g_images.find(img);
            if (it == g_images.end()) return;
            if (getenv("J2ME_TRACE_GLYPH") && sw == 9 && sh == 12) {
                static int s_g = 0;
                if (s_g < 60) {
                    fprintf(stderr, "[glyph] sx=%d sy=%d dx=%d dy=%d\n", sx, sy, dx, dy);
                    s_g++;
                }
            }
            // Compute drawn dimensions after transform
            int dw = sw, dh = sh;
            if (t == 4 || t == 5 || t == 6 || t == 7) { dw = sh; dh = sw; }
            if (anchor & 1)  dx -= dw / 2;  // HCENTER
            if (anchor & 8)  dx -= dw;       // RIGHT
            if (anchor & 2)  dy -= dh / 2;  // VCENTER
            if (anchor & 32) dy -= dh;       // BOTTOM
            auto tr = gfx_tx(gfx);
            blit_image(gfx_surface(gfx), it->second, dx + tr.x, dy + tr.y, sx, sy, sw, sh, t);
        });

    // Graphics.drawRGB(int[] rgb, int off, int scanlength,
    //                  int x, int y, int w, int h, boolean processAlpha)
    // Direct pixel composite. Doom RPG uses this for its bitmap-font menu
    // text — without an implementation, menu items render invisible.
    vm.register_native("javax/microedition/lcdui/Graphics",
        "drawRGB", "([IIIIIIIZ)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ObjRef arr  = args[1].as_ref();
            int off     = args[2].as_int();
            int scan    = args[3].as_int();
            int x       = args[4].as_int();
            int y       = args[5].as_int();
            int w       = args[6].as_int();
            int h       = args[7].as_int();
            bool processAlpha = (args[8].as_int() != 0);

            SDL_Surface* dst = gfx_surface(self);
            if (!dst || w <= 0 || h <= 0) return;
            HeapObject* obj = v.heap().deref(arr);
            if (!obj) return;
            Slot* elems = obj->array_slots();
            int32_t len = obj->array_length();
            auto tr = gfx_tx(self);
            SDL_Rect clip; SDL_GetClipRect(dst, &clip);
            int cx0 = clip.x, cy0 = clip.y;
            int cx1 = clip.x + clip.w, cy1 = clip.y + clip.h;

            SDL_LockSurface(dst);
            uint32_t* dpix = static_cast<uint32_t*>(dst->pixels);
            int dpitch = dst->pitch / 4;
            for (int row = 0; row < h; ++row) {
                int py = y + row + tr.y;
                if (py < cy0 || py >= cy1) continue;
                int base = off + row * scan;
                for (int col = 0; col < w; ++col) {
                    int px = x + col + tr.x;
                    if (px < cx0 || px >= cx1) continue;
                    int idx = base + col;
                    if (idx < 0 || idx >= len) continue;
                    uint32_t src = static_cast<uint32_t>(elems[idx].as_int());
                    uint8_t a = processAlpha ? (uint8_t)(src >> 24) : 0xFF;
                    if (a == 0) continue;  // fully transparent
                    if (a == 0xFF) {
                        dpix[py * dpitch + px] = 0xFF000000u | (src & 0x00FFFFFFu);
                    } else {
                        // alpha blend src over dest
                        uint32_t d = dpix[py * dpitch + px];
                        uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                        uint8_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
                        uint8_t r = (sr * a + dr * (255 - a)) / 255;
                        uint8_t g = (sg * a + dg * (255 - a)) / 255;
                        uint8_t b = (sb * a + db * (255 - a)) / 255;
                        dpix[py * dpitch + px] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                }
            }
            SDL_UnlockSurface(dst);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "copyArea", "(IIIIIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* s = gfx_surface(self);
            if (!s) return;
            auto t = gfx_tx(self);
            int sx = args[1].as_int() + t.x, sy = args[2].as_int() + t.y;
            int w  = args[3].as_int(), h = args[4].as_int();
            int dx = args[5].as_int() + t.x, dy = args[6].as_int() + t.y;
            int anchor = args[7].as_int();
            // Apply anchor to destination
            if (anchor & 1)  dx -= w / 2;
            if (anchor & 8)  dx -= w;
            if (anchor & 2)  dy -= h / 2;
            if (anchor & 32) dy -= h;
            // copyArea on same surface needs a temp buffer
            SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!tmp) return;
            SDL_Rect sr{sx, sy, w, h};
            SDL_BlitSurface(s, &sr, tmp, nullptr);
            SDL_Rect dr{dx, dy, w, h};
            SDL_BlitSurface(tmp, nullptr, s, &dr);
            SDL_FreeSurface(tmp);
            static int ca_count = 0;
            if (++ca_count <= 5)
                fprintf(stderr, "[gfx] copyArea: src=(%d,%d) dst=(%d,%d) size=%dx%d\n",
                        sx, sy, dx, dy, w, h);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "setFont", "(Ljavax/microedition/lcdui/Font;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_gfx_font[args[0].as_ref()] = args[1].as_ref();
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "getFont", "()Ljavax/microedition/lcdui/Font;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_gfx_font.find(args[0].as_ref());
            if (it != g_gfx_font.end()) { f.push_ref(it->second); return; }
            // Return a default font
            ObjRef ref = v.new_object(v.loader().find_or_stub("javax/microedition/lcdui/Font"));
            g_font_size[ref] = 0;
            g_font_style[ref] = 0;
            f.push_ref(ref);
        });

    vm.register_native("javax/microedition/lcdui/Graphics",
        "getStrokeStyle", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); /* SOLID */ });
    vm.register_noop("javax/microedition/lcdui/Graphics",
        "setStrokeStyle", "(I)V",
        "only solid strokes supported; dotted style ignored",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Font ─────────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Font",
        "getFont", "(III)Ljavax/microedition/lcdui/Font;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            int face  = args[0].as_int();  // FACE_SYSTEM=0, FACE_MONOSPACE=32, FACE_PROPORTIONAL=64
            int style = args[1].as_int();  // STYLE_PLAIN=0, BOLD=1, ITALIC=2
            int size  = args[2].as_int();  // SIZE_SMALL=8, SIZE_MEDIUM=0, SIZE_LARGE=16
            ObjRef ref = v.new_object(v.loader().find_or_stub("javax/microedition/lcdui/Font"));
            g_font_size[ref]  = size;
            g_font_style[ref] = style;
            g_font_face[ref]  = face;
            f.push_ref(ref);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getDefaultFont", "()Ljavax/microedition/lcdui/Font;",
        [](VM& v, Frame& f, std::span<Slot>) {
            ObjRef ref = v.new_object(v.loader().find_or_stub("javax/microedition/lcdui/Font"));
            g_font_size[ref]  = 0;  // SIZE_MEDIUM
            g_font_style[ref] = 0;  // STYLE_PLAIN
            f.push_ref(ref);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "stringWidth", "(Ljava/lang/String;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            std::string text = v.string_value(args[1].as_ref());
            TTF_Font* ttf = font_for_obj(self);
            if (ttf && !text.empty()) {
                int w = 0;
                TTF_SizeUTF8(ttf, text.c_str(), &w, nullptr);
                f.push_int(w);
            } else {
                f.push_int(0);
            }
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "substringWidth", "(Ljava/lang/String;II)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            std::string full = v.string_value(args[1].as_ref());
            int offset = args[2].as_int();
            int len    = args[3].as_int();
            TTF_Font* ttf = font_for_obj(self);
            if (!ttf || len <= 0) { f.push_int(0); return; }
            if (offset < 0) offset = 0;
            if (offset >= (int)full.size()) { f.push_int(0); return; }
            if (offset + len > (int)full.size()) len = (int)full.size() - offset;
            std::string sub = full.substr(offset, len);
            int w = 0;
            TTF_SizeUTF8(ttf, sub.c_str(), &w, nullptr);
            f.push_int(w);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "charsWidth", "([CII)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ObjRef arr_ref = args[1].as_ref();
            int offset = args[2].as_int();
            int len    = args[3].as_int();
            TTF_Font* ttf = font_for_obj(self);
            HeapObject* arr = v.heap().deref(arr_ref);
            if (!ttf || !arr || len <= 0) { f.push_int(0); return; }
            int alen = arr->array_length();
            if (offset < 0) offset = 0;
            if (offset + len > alen) len = alen - offset;
            std::string s;
            s.reserve(len);
            for (int i = 0; i < len; ++i) {
                int c = arr->array_shorts()[offset + i] & 0xFFFF;
                if (c < 0x80) s.push_back((char)c);
                else if (c < 0x800) {
                    s.push_back((char)(0xC0 | (c >> 6)));
                    s.push_back((char)(0x80 | (c & 0x3F)));
                } else {
                    s.push_back((char)(0xE0 | (c >> 12)));
                    s.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
                    s.push_back((char)(0x80 | (c & 0x3F)));
                }
            }
            int w = 0;
            TTF_SizeUTF8(ttf, s.c_str(), &w, nullptr);
            f.push_int(w);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "charWidth", "(C)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            TTF_Font* ttf = font_for_obj(self);
            if (ttf) {
                char buf[2] = { (char)args[1].as_int(), 0 };
                int w = 0;
                TTF_SizeUTF8(ttf, buf, &w, nullptr);
                f.push_int(w);
            } else {
                f.push_int(6);
            }
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            TTF_Font* ttf = font_for_obj(self);
            f.push_int(ttf ? TTF_FontHeight(ttf) : 12);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getBaselinePosition", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            TTF_Font* ttf = font_for_obj(self);
            f.push_int(ttf ? TTF_FontAscent(ttf) : 10);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getSize", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_font_size.find(args[0].as_ref());
            f.push_int(it != g_font_size.end() ? it->second : 0);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getStyle", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_font_style.find(args[0].as_ref());
            f.push_int(it != g_font_style.end() ? it->second : 0);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "getFace", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); /* FACE_SYSTEM */ });

    vm.register_native("javax/microedition/lcdui/Font",
        "isBold", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_font_style.find(args[0].as_ref());
            f.push_int((it != g_font_style.end() && (it->second & 1)) ? 1 : 0);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "isItalic", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_font_style.find(args[0].as_ref());
            f.push_int((it != g_font_style.end() && (it->second & 2)) ? 1 : 0);
        });

    vm.register_native("javax/microedition/lcdui/Font",
        "isPlain", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_font_style.find(args[0].as_ref());
            f.push_int((it == g_font_style.end() || it->second == 0) ? 1 : 0);
        });

    // ── Image ─────────────────────────────────────────────────────────────────

    auto make_image_ref = [](VM& v, SDL_Surface* surf) -> ObjRef {
        ClassDef* imgKlass = v.loader().find_or_stub("javax/microedition/lcdui/Image");
        ObjRef ref = v.heap().alloc_object(imgKlass, 0);
        g_images[ref] = surf;
        return ref;
    };

    // Image.createImage(InputStream) — read remaining bytes then decode as PNG
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage", "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            extern StreamEntry* find_stream(ObjRef ref);
            StreamEntry* s = find_stream(args[0].as_ref());
            if (!s || s->pos >= (int32_t)s->data.size()) { f.push_ref(NULL_REF); return; }
            const uint8_t* bytes = s->data.data() + s->pos;
            size_t len = s->data.size() - s->pos;
            SDL_Surface* surf = load_png_from_bytes(bytes, (int)len);
            s->pos = (int32_t)s->data.size();
            if (!surf) { f.push_ref(NULL_REF); return; }
            f.push_ref(make_image_ref(v, surf));
        });

    // Image.createImage(byte[], int, int)
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage", "([BII)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef arr = args[0].as_ref();
            int offset = args[1].as_int(), length = args[2].as_int();
            HeapObject* obj = v.heap().deref(arr);
            if (!obj) { f.push_ref(NULL_REF); return; }

            const uint8_t* data = obj->array_bytes() + offset;
            if (const char* dir = std::getenv("J2ME_DUMP_PNGS")) {
                // Unrelated-asset debug: dump raw PNG bytes to a directory
                // so a human can inspect what the game is feeding the
                // decoder. Zero-cost when the env var is unset.
                static int s_id = 0;
                char path[512];
                std::snprintf(path, sizeof(path), "%s/png_%04d_len%d.png",
                              dir, s_id++, length);
                FILE* fp = std::fopen(path, "wb");
                if (fp) { std::fwrite(data, 1, length, fp); std::fclose(fp); }
            }
            SDL_Surface* surf = load_png_from_bytes(data, length);
            if (!surf) {
                f.push_ref(NULL_REF); return;
            }
            f.push_ref(make_image_ref(v, surf));
        });

    // Image.createImage(String) — load from JAR resource
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;",
        [&jar, make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            std::string requested = v.string_value(args[0].as_ref());
            std::string path = jar.resolve(requested);
            if (path.empty()) {
                fprintf(stderr, "[image] not found in JAR: %s\n", requested.c_str());
                f.push_ref(NULL_REF); return;
            }

            const auto& bytes = jar.get(path);
            SDL_Surface* surf = load_png_from_bytes(bytes.data(), bytes.size());
            if (!surf) {
                fprintf(stderr, "[image] PNG decode failed for %s: %s\n",
                        path.c_str(), SDL_GetError());
                f.push_ref(NULL_REF); return;
            }
            f.push_ref(make_image_ref(v, surf));
        });

    // Image.createImage(int, int) — mutable blank image
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage", "(II)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            int w = args[0].as_int(), h = args[1].as_int();
            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
                0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) SDL_FillRect(surf, nullptr, 0xFFFFFFFF);
            f.push_ref(surf ? make_image_ref(v, surf) : NULL_REF);
        });

    // Image.getGraphics() — return a Graphics context that draws into this Image
    vm.register_native("javax/microedition/lcdui/Image",
        "getGraphics", "()Ljavax/microedition/lcdui/Graphics;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef img_ref = args[0].as_ref();
            auto it = g_images.find(img_ref);
            if (it == g_images.end()) {
                f.push_ref(NULL_REF); return;
            }
            ClassDef* gfxKlass = v.loader().find_or_stub("javax/microedition/lcdui/Graphics");
            ObjRef gfxRef = v.heap().alloc_object(gfxKlass, 0);
            g_gfx_surf[gfxRef] = it->second;
            g_colors[gfxRef]   = 0xFF000000u;
            f.push_ref(gfxRef);
        });

    // Image.createImage(Image) — copy
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage",
        "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_images.find(args[0].as_ref());
            if (it == g_images.end()) { f.push_ref(NULL_REF); return; }
            SDL_Surface* copy = SDL_ConvertSurface(it->second,
                                     it->second->format, 0);
            f.push_ref(copy ? make_image_ref(v, copy) : NULL_REF);
        });

    // Image.createImage(Image, int,int,int,int,int)
    vm.register_native("javax/microedition/lcdui/Image",
        "createImage",
        "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_images.find(args[0].as_ref());
            if (it == g_images.end()) { f.push_ref(NULL_REF); return; }
            SDL_Surface* src = it->second;
            int x = args[1].as_int(), y = args[2].as_int();
            int w = args[3].as_int(), h = args[4].as_int();
            int t = args[5].as_int();

            SDL_Surface* sub = SDL_CreateRGBSurfaceWithFormat(
                0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!sub) { f.push_ref(NULL_REF); return; }
            blit_image(sub, src, 0, 0, x, y, w, h, t);
            f.push_ref(make_image_ref(v, sub));
        });

    // Image.createRGBImage(int[], int, int, boolean) — create from ARGB pixel data
    vm.register_native("javax/microedition/lcdui/Image",
        "createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;",
        [make_image_ref](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef arr_ref = args[0].as_ref();
            int32_t w = args[1].as_int(), h = args[2].as_int();
            bool processAlpha = (args[3].as_int() != 0);
            HeapObject* arr = v.heap().deref(arr_ref);
            if (!arr || w <= 0 || h <= 0) { f.push_ref(NULL_REF); return; }

            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
                0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!surf) { f.push_ref(NULL_REF); return; }

            SDL_LockSurface(surf);
            uint32_t* dst = static_cast<uint32_t*>(surf->pixels);
            int32_t len = arr->array_length();
            Slot* elems = arr->array_slots();
            for (int32_t i = 0; i < std::min(w * h, len); ++i) {
                uint32_t pixel = static_cast<uint32_t>(elems[i].as_int());
                if (!processAlpha)
                    pixel |= 0xFF000000u;  // force fully opaque
                dst[i] = pixel;
            }
            SDL_UnlockSurface(surf);
            SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
            f.push_ref(make_image_ref(v, surf));
        });

    // Image.getWidth / getHeight
    vm.register_native("javax/microedition/lcdui/Image",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_images.find(args[0].as_ref());
            f.push_int(it != g_images.end() ? it->second->w : 0);
        });
    vm.register_native("javax/microedition/lcdui/Image",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_images.find(args[0].as_ref());
            f.push_int(it != g_images.end() ? it->second->h : 0);
        });

    // Image.isMutable — we don't distinguish mutable/immutable yet; return
    // false (immutable) so games that gate drawing on mutability take the
    // "createImage then blit" path rather than "modify in place".
    vm.register_native("javax/microedition/lcdui/Image",
        "isMutable", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });

    // Image.getRGB(int[] rgb, int off, int scanlen, int x, int y, int w, int h)
    // Read a rectangle of pixels out of the image into an int[] as 0xAARRGGBB.
    // Doom II RPG uses this to sample its bitmap font atlas for stats/menu text.
    vm.register_native("javax/microedition/lcdui/Image",
        "getRGB", "([IIIIIII)V",
        [](VM& vm_, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ObjRef buf  = args[1].as_ref();
            int32_t off  = args[2].as_int();
            int32_t scan = args[3].as_int();
            int32_t x    = args[4].as_int();
            int32_t y    = args[5].as_int();
            int32_t w    = args[6].as_int();
            int32_t h    = args[7].as_int();
            auto it = g_images.find(self);
            if (it == g_images.end()) return;
            SDL_Surface* src = it->second;
            HeapObject* arr = vm_.heap().deref(buf);
            if (!arr) return;
            int32_t alen = arr->array_length();
            Slot* dst = arr->array_slots();
            SDL_LockSurface(src);
            uint32_t* pixels = (uint32_t*)src->pixels;
            int pitch = src->pitch / 4;
            for (int row = 0; row < h; ++row) {
                int sy = y + row;
                for (int col = 0; col < w; ++col) {
                    int sx = x + col;
                    int idx = off + row * scan + col;
                    if (idx < 0 || idx >= alen) continue;
                    uint32_t p = 0;
                    if (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h) {
                        p = pixels[sy * pitch + sx];
                        uint8_t r, g, b, a;
                        SDL_GetRGBA(p, src->format, &r, &g, &b, &a);
                        p = (uint32_t(a) << 24) | (uint32_t(r) << 16)
                          | (uint32_t(g) << 8)  |  uint32_t(b);
                    }
                    dst[idx] = Slot::from_int((int32_t)p);
                }
            }
            SDL_UnlockSurface(src);
        });

    // Image.getGraphics() — mutable image → get a Graphics for it
    vm.register_native("javax/microedition/lcdui/Image",
        "getGraphics", "()Ljavax/microedition/lcdui/Graphics;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef img = args[0].as_ref();
            auto it = g_images.find(img);
            if (it == g_images.end()) { f.push_ref(NULL_REF); return; }

            ClassDef* gfxKlass = v.loader().find_or_stub("javax/microedition/lcdui/Graphics");
            ObjRef gfxRef = v.heap().alloc_object(gfxKlass, 0);
            g_gfx_surf[gfxRef] = it->second;
            g_colors[gfxRef]   = 0xFF000000u;
            f.push_ref(gfxRef);
        });

    // ── GameCanvas / Canvas ───────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "getGraphics", "()Ljavax/microedition/lcdui/Graphics;",
        [](VM& v, Frame& f, std::span<Slot>) {
            Display& d = Display::instance();
            if (!d.is_open()) d.open(g_screen_w, g_screen_h);

            ClassDef* gfxKlass = v.loader().find_or_stub("javax/microedition/lcdui/Graphics");
            ObjRef gfxRef = v.heap().alloc_object(gfxKlass, 0);
            g_gfx_surf[gfxRef] = d.screen();
            g_colors[gfxRef]   = 0xFF000000u;
            f.push_ref(gfxRef);
        });

    // Helper: deliver pending pointer events to a Canvas object
    auto deliver_pointer_events = [](VM& v, ObjRef canvas_ref) {
        Display& d = Display::instance();
        auto events = d.take_pointer_events();
        if (events.empty()) return;
        HeapObject* canvas_obj = v.heap().deref(canvas_ref);
        if (!canvas_obj || !canvas_obj->klass) return;
        MethodDef* pp = canvas_obj->klass->resolve_virtual("pointerPressed",  "(II)V");
        MethodDef* pr = canvas_obj->klass->resolve_virtual("pointerReleased", "(II)V");
        MethodDef* pd = canvas_obj->klass->resolve_virtual("pointerDragged",  "(II)V");
        for (auto& e : events) {
            MethodDef* m = nullptr;
            switch (e.kind) {
                case Display::PointerKind::Pressed:  m = pp; break;
                case Display::PointerKind::Released: m = pr; break;
                case Display::PointerKind::Dragged:  m = pd; break;
            }
            if (!m) continue;
            try {
                v.invoke(m, canvas_obj->klass, {
                    Slot::from_ref(canvas_ref),
                    Slot::from_int(e.x),
                    Slot::from_int(e.y)
                });
            } catch (const QuitRequest&) { throw; }
            catch (...) {}
        }
    };

    // Canvas.hasPointerEvents() / hasPointerMotionEvents() — emulate touchscreen
    vm.register_native("javax/microedition/lcdui/Canvas",
        "hasPointerEvents", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "hasPointerMotionEvents", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });

    // Helper: deliver pending key press/release events to a GameCanvas/Canvas object
    auto deliver_key_events = [deliver_pointer_events](VM& v, ObjRef canvas_ref) {
        deliver_pointer_events(v, canvas_ref);
        Display& d = Display::instance();
        auto presses  = d.take_key_presses();
        auto releases = d.take_key_releases();

        if (presses.empty() && releases.empty()) return;

        HeapObject* canvas_obj = v.heap().deref(canvas_ref);
        if (!canvas_obj || !canvas_obj->klass) return;

        MethodDef* kp = canvas_obj->klass->resolve_virtual("keyPressed",  "(I)V");
        MethodDef* kr = canvas_obj->klass->resolve_virtual("keyReleased", "(I)V");
        if (std::getenv("J2ME_TRACE_KEYS"))
            fprintf(stderr, "[key] delivering %zu presses + %zu releases to %s (kp=%p kr=%p)\n",
                    presses.size(), releases.size(),
                    canvas_obj->klass->name.c_str(), (void*)kp, (void*)kr);

        for (int code : presses) {
            // Soft keys: fire commandAction on the CommandListener if registered
            if (code == -6 || code == -7) {
                auto clit = g_command_listeners.find(canvas_ref);
                auto cmit = g_commands.find(canvas_ref);
                if (clit != g_command_listeners.end() && clit->second != NULL_REF &&
                    cmit != g_commands.end() && !cmit->second.empty()) {
                    // -6 = left soft key → first command, -7 = right soft key → last command
                    ObjRef cmd = (code == -6) ? cmit->second.front() : cmit->second.back();
                    HeapObject* lobj = v.heap().deref(clit->second);
                    if (lobj && lobj->klass) {
                        MethodDef* ca = lobj->klass->resolve_virtual(
                            "commandAction",
                            "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                        if (ca) try {
                            v.invoke(ca, lobj->klass, {
                                Slot::from_ref(clit->second),
                                Slot::from_ref(cmd),
                                Slot::from_ref(canvas_ref)
                            });
                        } catch (const QuitRequest&) { throw;
                        } catch (...) {}
                    }
                }
            }
            if (kp) try {
                v.invoke(kp, canvas_obj->klass,
                         {Slot::from_ref(canvas_ref), Slot::from_int(code)});
            } catch (const QuitRequest&) { throw;
            } catch (...) {}
        }

        for (int code : releases) {
            if (kr) try {
                v.invoke(kr, canvas_obj->klass,
                         {Slot::from_ref(canvas_ref), Slot::from_int(code)});
            } catch (const QuitRequest&) { throw;
            } catch (...) {}
        }
    };

    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "flushGraphics", "()V",
        [deliver_key_events](VM& v, Frame&, std::span<Slot> args) {
            static bool first = false;
            if (!first) { first = true; fprintf(stderr, "[survey] first-flush kind=GameCanvas\n"); }
            if (!Display::instance().flush()) {
                throw QuitRequest{};
            }
            deliver_key_events(v, args[0].as_ref());
        });

    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "flushGraphics", "(IIII)V",
        [deliver_key_events](VM& v, Frame&, std::span<Slot> args) {
            static bool first = false;
            if (!first) { first = true; fprintf(stderr, "[survey] first-flush kind=GameCanvas-rect\n"); }
            if (!Display::instance().flush()) {
                throw QuitRequest{};
            }
            deliver_key_events(v, args[0].as_ref());
        });

    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "getKeyStates", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Display::instance().key_states());
        });

    // GameCanvas.getGraphics() returns a Graphics that draws into the
    // offscreen buffer; flushGraphics() copies buffer->screen. We don't
    // maintain a separate offscreen buffer — drawing goes directly to the
    // screen surface, and flushGraphics just presents it. Visually that's
    // the same as an immediate-paint Canvas. Games relying on this pattern
    // (Doom RPG, likely many others) silently produced NULL Graphics and
    // dropped every draw before this was wired up.
    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "getGraphics", "()Ljavax/microedition/lcdui/Graphics;",
        [](VM& v, Frame& f, std::span<Slot>) {
            Display& d = Display::instance();
            if (!d.is_open()) d.open(g_screen_w, g_screen_h);
            ClassDef* gfxKlass = v.loader().find_or_stub(
                "javax/microedition/lcdui/Graphics");
            ObjRef gfxRef = v.heap().alloc_object(gfxKlass, 0);
            g_gfx_surf[gfxRef] = d.screen();
            g_colors[gfxRef]   = 0xFF000000u;
            f.push_ref(gfxRef);
        });

    vm.register_native("javax/microedition/lcdui/Canvas",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            // Report g_screen_w (the logical canvas) rather than
            // Display::width(), which is 240 until the window opens and would
            // make the game lay out at 240 if called during startup.
            f.push_int(g_screen_w);
        });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(g_screen_h);
        });
    vm.register_native("javax/microedition/lcdui/Displayable",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(g_screen_w);
        });
    vm.register_native("javax/microedition/lcdui/Displayable",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(g_screen_h);
        });
    // Helper lambda shared by repaint() and serviceRepaints()
    auto do_repaint = [deliver_key_events](VM& v, ObjRef canvas_ref) {
        HeapObject* canvas_obj = v.heap().deref(canvas_ref);
        if (!canvas_obj || !canvas_obj->klass) return;

        Display& d = Display::instance();
        if (!d.is_open()) d.open(g_screen_w, g_screen_h);

        // Allocate a Graphics backed by the screen surface
        ClassDef* gfxKlass = v.loader().find_or_stub("javax/microedition/lcdui/Graphics");
        ObjRef gfxRef = v.heap().alloc_object(gfxKlass, 0);
        g_gfx_surf[gfxRef] = d.screen();
        g_colors[gfxRef]   = 0xFF000000u;

        // Call paint(Graphics) on the canvas
        MethodDef* paint = canvas_obj->klass->resolve_virtual(
            "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
        if (paint) {
            static bool first_paint_logged = false;
            if (!first_paint_logged) {
                first_paint_logged = true;
                fprintf(stderr, "[survey] first-paint klass=%s\n",
                        canvas_obj->klass->name.c_str());
            }
            try {
                v.invoke(paint, canvas_obj->klass,
                         {Slot::from_ref(canvas_ref), Slot::from_ref(gfxRef)});
            } catch (const QuitRequest&) { throw;
            } catch (const JvmException& e) {
                fprintf(stderr, "[repaint] paint threw: %s at %s\n",
                        e.message.c_str(), e.location.c_str());
            } catch (const std::exception& e) {
                fprintf(stderr, "[repaint] paint threw C++: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "[repaint] paint threw unknown exception\n");
            }
        }

        if (!d.flush()) throw QuitRequest{};

        // Deliver key press/release events that arrived this frame to the canvas.
        deliver_key_events(v, canvas_ref);
    };

    vm.register_native("javax/microedition/lcdui/Canvas",
        "repaint", "()V",
        [do_repaint](VM& v, Frame&, std::span<Slot> args) {
            do_repaint(v, args[0].as_ref());
        });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "repaint", "(IIII)V",
        [do_repaint](VM& v, Frame&, std::span<Slot> args) {
            do_repaint(v, args[0].as_ref());
        });

    // serviceRepaints() — force any pending repaint synchronously (same as repaint in our model)
    vm.register_native("javax/microedition/lcdui/Canvas",
        "serviceRepaints", "()V",
        [do_repaint](VM& v, Frame&, std::span<Slot> args) {
            do_repaint(v, args[0].as_ref());
        });

    // isShown() — our canvas is always visible once the MIDlet starts
    vm.register_native("javax/microedition/lcdui/Displayable",
        "isShown", "()Z",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(1);  // always shown
        });

    // ── Canvas/Displayable stubs ───────────────────────────────────────────────

    vm.register_noop("javax/microedition/lcdui/Canvas",
        "<init>", "()V",
        "no Canvas-level fields to init",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Nokia FullCanvas ─────────────────────────────────────────────────────
    // FullCanvas extends Canvas with full screen mode. Link the class hierarchy
    // so that subclasses of FullCanvas inherit Canvas methods.
    {
        ClassDef* fullCanvas = vm.loader().find_or_stub("com/nokia/mid/ui/FullCanvas");
        ClassDef* canvas = vm.loader().find_or_stub("javax/microedition/lcdui/Canvas");
        if (!fullCanvas->super) fullCanvas->super = canvas;
    }
    vm.register_noop("com/nokia/mid/ui/FullCanvas",
        "<init>", "()V",
        "no fields to init; we run full-screen by default",
        [](VM&, Frame&, std::span<Slot>) {});

    // Nokia Sound stub
    vm.register_stub("com/nokia/mid/sound/Sound",
        "<init>", "([BI)V",
        "Nokia OTT tone format not decoded; sound silently created",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_stub("com/nokia/mid/sound/Sound",
        "play", "(I)V",
        "Nokia Sound decoding not implemented; silent",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_stub("com/nokia/mid/sound/Sound",
        "stop", "()V",
        "Nokia Sound not playing anyway",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_native("javax/microedition/lcdui/Canvas",
        "isDoubleBuffered", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });

    vm.register_native("javax/microedition/lcdui/Displayable",
        "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_command_listeners[args[0].as_ref()] = args[1].as_ref();
        });

    vm.register_native("javax/microedition/lcdui/Displayable",
        "addCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_commands[args[0].as_ref()].push_back(args[1].as_ref());
        });

    vm.register_native("javax/microedition/lcdui/Displayable",
        "removeCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& cmds = g_commands[args[0].as_ref()];
            auto it = std::find(cmds.begin(), cmds.end(), args[1].as_ref());
            if (it != cmds.end()) cmds.erase(it);
        });

    vm.register_native("javax/microedition/lcdui/Command",
        "<init>", "(Ljava/lang/String;II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            CommandInfo& ci = g_command_info[args[0].as_ref()];
            ci.label    = args[1].as_ref();
            ci.type     = args[2].as_int();
            ci.priority = args[3].as_int();
        });

    vm.register_native("javax/microedition/lcdui/Command",
        "getLabel", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_command_info.find(args[0].as_ref());
            f.push_ref(it != g_command_info.end() ? it->second.label : NULL_REF);
        });

    vm.register_native("javax/microedition/lcdui/Command",
        "getCommandType", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_command_info.find(args[0].as_ref());
            f.push_int(it != g_command_info.end() ? it->second.type : 0);
        });

    vm.register_native("javax/microedition/lcdui/Command",
        "getPriority", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_command_info.find(args[0].as_ref());
            f.push_int(it != g_command_info.end() ? it->second.priority : 0);
        });

    // ── Timer / TimerTask ────────────────────────────────────────────────────
    // J2ME Timer.schedule(task, delay, period) runs task.run() repeatedly.
    // We implement this as a loop inside the deferred thread system.

    vm.register_noop("java/util/TimerTask", "<init>", "()V",
        "state captured on schedule(); nothing to init here",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_noop("java/util/Timer", "<init>", "()V",
        "Timer backed by scheduler-thread loops; no init state",
        [](VM&, Frame&, std::span<Slot>) {});

    // Timer state: store task + period for the timer loop runner
    struct TimerInfo {
        ObjRef task_ref;
        MethodDef* run;
        ClassDef* klass;
        int64_t period_ms;
        bool repeating;
        bool cancelled;
    };
    static std::unordered_map<ObjRef, std::shared_ptr<TimerInfo>> g_timers;

    // Pre-register a single repeating timer runner to avoid dynamic registration
    static std::shared_ptr<TimerInfo> g_pending_repeating;
    {
        ClassDef* loopKlass = vm.loader().find_or_stub("__timer_loop__");
        vm.register_native("__timer_loop__", "__repeating__", "()V",
            [](VM& v2, Frame&, std::span<Slot>) {
                auto info = g_pending_repeating;
                if (!info) return;
                g_pending_repeating = nullptr;
                while (!info->cancelled) {
                    try {
                        v2.invoke(info->run, info->klass,
                                  {Slot::from_ref(info->task_ref)});
                    } catch (const QuitRequest&) { throw; }
                    catch (...) { break; }
                    SDL_Delay((uint32_t)std::min(info->period_ms, int64_t(50)));
                    if (!Display::instance().flush())
                        throw QuitRequest{};
                }
            });
    }

    auto start_repeating_timer = [](VM& v, ObjRef timer_ref, ObjRef task_ref,
                                    int64_t period_ms) {
        if (period_ms < 16) period_ms = 16;
        HeapObject* task_obj = v.heap().deref(task_ref);
        if (!task_obj || !task_obj->klass) return;
        MethodDef* run = task_obj->klass->resolve_virtual("run", "()V");
        if (!run) return;
        auto info = std::make_shared<TimerInfo>(
            TimerInfo{task_ref, run, task_obj->klass, period_ms, true, false});
        g_timers[timer_ref] = info;
        g_pending_repeating = info;
        ClassDef* loopKlass = v.loader().find_or_stub("__timer_loop__");
        MethodDef* md = loopKlass->find_method("__repeating__", "()V");
        ObjRef dummy = v.new_object(loopKlass);
        v.start_thread(dummy, dummy, md, loopKlass);
    };

    // schedule(TimerTask, long delay, long period)
    vm.register_native("java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;JJ)V",
        [start_repeating_timer](VM& v, Frame&, std::span<Slot> args) {
            ObjRef timer_ref = args[0].as_ref();
            ObjRef task_ref = args[1].as_ref();
            Slot2 ps; ps.lo = args[4].raw; ps.hi = args[5].raw;
            start_repeating_timer(v, timer_ref, task_ref, ps.as_long());
        });

    // scheduleAtFixedRate(TimerTask, long delay, long period)
    vm.register_native("java/util/Timer", "scheduleAtFixedRate",
        "(Ljava/util/TimerTask;JJ)V",
        [start_repeating_timer](VM& v, Frame&, std::span<Slot> args) {
            ObjRef timer_ref = args[0].as_ref();
            ObjRef task_ref = args[1].as_ref();
            Slot2 ps; ps.lo = args[4].raw; ps.hi = args[5].raw;
            start_repeating_timer(v, timer_ref, task_ref, ps.as_long());
        });

    // schedule(TimerTask, long delay) — one-shot with delay
    // We pre-register a single "oneshot runner" method to avoid dynamic registration
    // which causes use-after-free when the methods vector reallocates.
    static std::shared_ptr<TimerInfo> g_pending_oneshot;
    {
        ClassDef* loopKlass = vm.loader().find_or_stub("__timer_loop__");
        vm.register_native("__timer_loop__", "__oneshot__", "()V",
            [](VM& v2, Frame&, std::span<Slot>) {
                auto info = g_pending_oneshot;
                if (!info) return;
                g_pending_oneshot = nullptr;
                // Wait with display updates
                int64_t remaining = info->period_ms;
                while (remaining > 0 && !info->cancelled) {
                    int64_t step = std::min(remaining, int64_t(50));
                    SDL_Delay((uint32_t)step);
                    remaining -= step;
                    if (!Display::instance().flush())
                        throw QuitRequest{};
                }
                if (!info->cancelled) {
                    try {
                        v2.invoke(info->run, info->klass,
                                  {Slot::from_ref(info->task_ref)});
                    } catch (const QuitRequest&) { throw; }
                    catch (...) {}
                }
            });
    }
    vm.register_native("java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;J)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef task_ref = args[1].as_ref();
            Slot2 ds; ds.lo = args[2].raw; ds.hi = args[3].raw;
            int64_t delay_ms = ds.as_long();

            HeapObject* task_obj = v.heap().deref(task_ref);
            if (!task_obj || !task_obj->klass) return;
            MethodDef* run = task_obj->klass->resolve_virtual("run", "()V");
            if (!run) return;

            if (delay_ms > 100) {
                g_pending_oneshot = std::make_shared<TimerInfo>(
                    TimerInfo{task_ref, run, task_obj->klass, delay_ms, false, false});
                ClassDef* loopKlass = v.loader().find_or_stub("__timer_loop__");
                MethodDef* md = loopKlass->find_method("__oneshot__", "()V");
                ObjRef dummy = v.new_object(loopKlass);
                v.start_thread(dummy, dummy, md, loopKlass);
            } else {
                v.start_thread(task_ref, task_ref, run, task_obj->klass);
            }
        });

    vm.register_native("java/util/Timer", "cancel", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_timers.find(args[0].as_ref());
            if (it != g_timers.end()) it->second->cancelled = true;
        });

    vm.register_native("java/util/TimerTask", "cancel", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });

    vm.register_native("java/util/TimerTask", "scheduledExecutionTime", "()J",
        [](VM&, Frame& f, std::span<Slot>) {
            // Return current time — close enough for our single-threaded model
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            f.push_long(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
        });

    // ── Thread: deferred execution ────────────────────────────────────────────
    // Real J2ME threads are asynchronous — Thread.start() returns immediately and
    // the body runs later.  Game code relies on this: constructors call start()
    // before singleton fields are set, then run() reads those fields.  Our
    // single-threaded emulator defers threads until vm.run() drains them after
    // startApp() returns.

    // Thread(Runnable) constructor: store the runnable for later start()
    vm.register_native("java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef thread_ref   = args[0].as_ref();
            ObjRef runnable_ref = args[1].as_ref();
            g_thread_runnable[thread_ref] = runnable_ref;
        });

    vm.register_native("java/lang/Thread", "start", "()V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef thread_ref = args[0].as_ref();

            // Pattern 1: new Thread(runnable) — stored in g_thread_runnable
            // Pattern 2: class Foo extends Thread { run() {} } — thread IS the runnable
            ObjRef runnable_ref = thread_ref;
            auto it = g_thread_runnable.find(thread_ref);
            if (it != g_thread_runnable.end())
                runnable_ref = it->second;

            HeapObject* runnable_obj = v.heap().deref(runnable_ref);
            if (!runnable_obj || !runnable_obj->klass) return;

            MethodDef* run = runnable_obj->klass->resolve_virtual("run", "()V");
            if (!run) {
                fprintf(stderr, "[thread] start(): no run() on klass=%s\n",
                        runnable_obj->klass->name.c_str());
                return;
            }

            // Spawn a new green thread; it becomes Ready and runs when the
            // current thread next yields (sleep/wait).
            v.start_thread(thread_ref, runnable_ref, run, runnable_obj->klass);
        });

    vm.register_native("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.current_thread_ref());
        });

    vm.register_native("java/lang/Thread", "sleep", "(J)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            // Static method: args[0]=lo, args[1]=hi of the long milliseconds argument
            Slot2 s; s.lo = args[0].raw; s.hi = args[1].raw;
            int64_t ms = s.as_long();
            if (getenv("J2ME_TRACE_SLEEP_CALLER")) {
                // Walk up the current thread's frame stack to see who's calling.
                auto* jt = v.scheduler.current();
                if (jt) {
                    fprintf(stderr, "[sleep caller] ms=%ld frames:\n", (long)ms);
                    int d = 0;
                    for (auto it = jt->frames.rbegin(); it != jt->frames.rend() && d < 5; ++it, ++d) {
                        fprintf(stderr, "  %s.%s pc=%u\n",
                                it->klass ? it->klass->name.c_str() : "?",
                                it->method ? it->method->name.c_str() : "?",
                                it->pc);
                    }
                }
            }
            if (ms <= 0) ms = 1;  // 0 means "yield" in our model
            // Sleep on the green-thread scheduler, not SDL_Delay. This
            // blocks only THIS thread — other Java threads get to run.
            // The scheduler's main loop polls SDL events between dispatches
            // so keyboard/window-close still work while we're here.
            v.scheduler.sleep_current((uint64_t)ms);
            if (!Display::instance().flush())
                throw QuitRequest{};
        });

    // ── javax.microedition.lcdui.game.Sprite + Layer ─────────────────────────
    // Sprite is the dominant 2D-game class in the MIDP corpus (1442+ JARs use
    // its <init>(Image,II) alone). Layer is the abstract base; Sprite extends
    // it. We track everything in one SpriteData per ObjRef and have Layer's
    // accessors read the same data — Sprite IS-A Layer at the bytecode level.
    struct SpriteData {
        SDL_Surface* img = nullptr;  // not owned; points into g_images
        int frame_w = 0, frame_h = 0;
        int cols    = 1;             // frames per row in the strip
        int frame_count = 1;
        int x = 0, y = 0;            // top-left position (in target coords)
        int ref_x = 0, ref_y = 0;    // reference pixel (in frame-local coords)
        int frame = 0;               // index into seq[]
        int transform = 0;           // MIDP TRANS_* constant
        bool visible = true;
        std::vector<int> seq;        // raw-frame indices; default 0..frame_count-1
    };
    static std::unordered_map<ObjRef, SpriteData> g_sprites;

    auto sprite_for = [](ObjRef ref) -> SpriteData* {
        auto it = g_sprites.find(ref);
        return it == g_sprites.end() ? nullptr : &it->second;
    };

    auto sprite_init_strip = [](SpriteData& sd, SDL_Surface* img,
                                int fw, int fh) {
        sd.img = img;
        sd.frame_w = fw > 0 ? fw : (img ? img->w : 0);
        sd.frame_h = fh > 0 ? fh : (img ? img->h : 0);
        sd.cols = (sd.frame_w > 0 && img) ? (img->w / sd.frame_w) : 1;
        int rows = (sd.frame_h > 0 && img) ? (img->h / sd.frame_h) : 1;
        sd.frame_count = sd.cols * rows;
        if (sd.frame_count <= 0) sd.frame_count = 1;
        sd.seq.resize(sd.frame_count);
        for (int i = 0; i < sd.frame_count; ++i) sd.seq[i] = i;
        sd.frame = 0;
    };

    // Sprite(Image)
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "<init>", "(Ljavax/microedition/lcdui/Image;)V",
        [sprite_init_strip](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* img = nullptr;
            auto it = g_images.find(args[1].as_ref());
            if (it != g_images.end()) img = it->second;
            SpriteData& sd = g_sprites[self];
            sprite_init_strip(sd, img, img ? img->w : 0, img ? img->h : 0);
        });

    // Sprite(Image, frameWidth, frameHeight)
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "<init>", "(Ljavax/microedition/lcdui/Image;II)V",
        [sprite_init_strip](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            SDL_Surface* img = nullptr;
            auto it = g_images.find(args[1].as_ref());
            if (it != g_images.end()) img = it->second;
            int fw = args[2].as_int(), fh = args[3].as_int();
            SpriteData& sd = g_sprites[self];
            sprite_init_strip(sd, img, fw, fh);
        });

    // Sprite(Sprite) — copy ctor; rarely used but games occasionally clone
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "<init>", "(Ljavax/microedition/lcdui/game/Sprite;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), src = args[1].as_ref();
            auto it = g_sprites.find(src);
            if (it != g_sprites.end()) g_sprites[self] = it->second;
        });

    // setFrame / getFrame / getRawFrameCount / getFrameSequenceLength
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setFrame", "(I)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd || sd->seq.empty()) return;
            int n = args[1].as_int();
            if (n < 0) n = 0;
            if (n >= (int)sd->seq.size()) n = (int)sd->seq.size() - 1;
            sd->frame = n;
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getFrame", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->frame : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getRawFrameCount", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->frame_count : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getFrameSequenceLength", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? (int)sd->seq.size() : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setFrameSequence", "([I)V",
        [sprite_for](VM& v, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd) return;
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) {
                // Reset to identity sequence
                sd->seq.resize(sd->frame_count);
                for (int i = 0; i < sd->frame_count; ++i) sd->seq[i] = i;
            } else {
                HeapObject* a = v.heap().deref(arr);
                if (!a) return;
                int n = a->array_length();
                Slot* s = a->array_slots();
                sd->seq.resize(n);
                for (int i = 0; i < n; ++i) sd->seq[i] = s[i].as_int();
            }
            sd->frame = 0;
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "nextFrame", "()V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd || sd->seq.empty()) return;
            sd->frame = (sd->frame + 1) % (int)sd->seq.size();
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "prevFrame", "()V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd || sd->seq.empty()) return;
            sd->frame = (sd->frame + (int)sd->seq.size() - 1) % (int)sd->seq.size();
        });

    // Transform
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setTransform", "(I)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) sd->transform = args[1].as_int();
        });

    // Reference pixel
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "defineReferencePixel", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) { sd->ref_x = args[1].as_int(); sd->ref_y = args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setRefPixelPosition", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd) return;
            // Spec: position the sprite so the reference pixel ends up at (x,y)
            sd->x = args[1].as_int() - sd->ref_x;
            sd->y = args[2].as_int() - sd->ref_y;
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getRefPixelX", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? (sd->x + sd->ref_x) : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getRefPixelY", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? (sd->y + sd->ref_y) : 0);
        });

    // Layer base-class accessors — all keyed off the same SpriteData map
    // (Sprite extends Layer; both use the same ObjRef).
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "setPosition", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) { sd->x = args[1].as_int(); sd->y = args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "move", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) { sd->x += args[1].as_int(); sd->y += args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "getX", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->x : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "getY", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->y : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "getWidth", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            // Rotated transforms swap w/h
            if (sd && (sd->transform == 4 || sd->transform == 5 ||
                       sd->transform == 6 || sd->transform == 7))
                f.push_int(sd->frame_h);
            else
                f.push_int(sd ? sd->frame_w : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "getHeight", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd && (sd->transform == 4 || sd->transform == 5 ||
                       sd->transform == 6 || sd->transform == 7))
                f.push_int(sd->frame_w);
            else
                f.push_int(sd ? sd->frame_h : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "setVisible", "(Z)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) sd->visible = args[1].as_int() != 0;
        });
    vm.register_native("javax/microedition/lcdui/game/Layer",
        "isVisible", "()Z",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd && sd->visible ? 1 : 0);
        });

    // Layer methods on Sprite directly. Our existing Layer registrations are
    // hierarchy-resolved at runtime, but the scanner (and some early-bound
    // bytecode) looks up constant-pool methodrefs on the exact class name —
    // so duplicate the registrations on Sprite specifically.
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setPosition", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) { sd->x = args[1].as_int(); sd->y = args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "move", "(II)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) { sd->x += args[1].as_int(); sd->y += args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getX", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->x : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getY", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd ? sd->y : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getWidth", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd && (sd->transform == 4 || sd->transform == 5 ||
                       sd->transform == 6 || sd->transform == 7))
                f.push_int(sd->frame_h);
            else
                f.push_int(sd ? sd->frame_w : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "getHeight", "()I",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd && (sd->transform == 4 || sd->transform == 5 ||
                       sd->transform == 6 || sd->transform == 7))
                f.push_int(sd->frame_w);
            else
                f.push_int(sd ? sd->frame_h : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setVisible", "(Z)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (sd) sd->visible = args[1].as_int() != 0;
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "isVisible", "()Z",
        [sprite_for](VM&, Frame& f, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            f.push_int(sd && sd->visible ? 1 : 0);
        });

    // Sprite.setImage: change backing image (and optionally re-grid frames).
    // Spec: preserves current frame and ref-pixel if the new frame grid has
    // at least the current frame index.
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "setImage", "(Ljavax/microedition/lcdui/Image;II)V",
        [sprite_for, sprite_init_strip](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd) return;
            SDL_Surface* img = nullptr;
            auto it = g_images.find(args[1].as_ref());
            if (it != g_images.end()) img = it->second;
            int fw = args[2].as_int(), fh = args[3].as_int();
            // Preserve x, y, ref pixel, transform, visibility across re-init.
            int save_x = sd->x, save_y = sd->y;
            int save_rx = sd->ref_x, save_ry = sd->ref_y;
            int save_t = sd->transform;
            bool save_v = sd->visible;
            int save_frame = sd->frame;
            sprite_init_strip(*sd, img, fw, fh);
            sd->x = save_x; sd->y = save_y;
            sd->ref_x = save_rx; sd->ref_y = save_ry;
            sd->transform = save_t;
            sd->visible = save_v;
            if (save_frame < (int)sd->seq.size()) sd->frame = save_frame;
        });

    // The actual draw — locate the current frame in the strip, route through
    // blit_image (which already handles all 8 MIDP transforms + clipping).
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "paint", "(Ljavax/microedition/lcdui/Graphics;)V",
        [sprite_for](VM&, Frame&, std::span<Slot> args) {
            SpriteData* sd = sprite_for(args[0].as_ref());
            if (!sd || !sd->visible || !sd->img) return;
            ObjRef gfx = args[1].as_ref();
            SDL_Surface* dst = gfx_surface(gfx);
            if (!dst) return;
            int raw = (sd->frame >= 0 && sd->frame < (int)sd->seq.size())
                        ? sd->seq[sd->frame] : 0;
            if (raw < 0 || raw >= sd->frame_count) raw = 0;
            int sx = (raw % sd->cols) * sd->frame_w;
            int sy = (raw / sd->cols) * sd->frame_h;
            auto t = gfx_tx(gfx);
            blit_image(dst, sd->img, sd->x + t.x, sd->y + t.y,
                       sx, sy, sd->frame_w, sd->frame_h, sd->transform);
        });

    // ── javax.microedition.lcdui.game.TiledLayer ──────────────────────────────
    // Tile-map renderer — the other half of the game-API 2D stack alongside
    // Sprite. A TiledLayer is a (cols × rows) grid where each cell holds an
    // index into a tile strip cut from a source Image. Heavily used by
    // platformers, RPG overworlds, and isometric tile puzzles.
    //
    // Cell values:
    //   0       — empty cell, draw nothing
    //   1..N    — static tile (1-based index into the strip)
    //   <0      — animated-tile id; lookup current static tile via setAnimatedTile
    struct TiledData {
        SDL_Surface* img = nullptr;       // not owned; into g_images
        int tile_w = 0, tile_h = 0;
        int strip_cols = 1;               // tiles per row in source image
        int static_tile_count = 0;        // count of base (1..N) tiles
        int cols = 0, rows = 0;           // map dimensions
        int x = 0, y = 0;
        bool visible = true;
        std::vector<int> cells;           // row-major; size = cols*rows
        std::vector<int> animated;        // animated[id-1] -> static tile index
    };
    static std::unordered_map<ObjRef, TiledData> g_tiled;
    auto tiled_for = [](ObjRef ref) -> TiledData* {
        auto it = g_tiled.find(ref);
        return it == g_tiled.end() ? nullptr : &it->second;
    };

    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "<init>", "(IILjavax/microedition/lcdui/Image;II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            int cols = args[1].as_int();
            int rows = args[2].as_int();
            SDL_Surface* img = nullptr;
            auto it = g_images.find(args[3].as_ref());
            if (it != g_images.end()) img = it->second;
            int tw = args[4].as_int(), th = args[5].as_int();
            TiledData& td = g_tiled[self];
            td.img = img;
            td.tile_w = tw;
            td.tile_h = th;
            td.strip_cols = (img && tw > 0) ? (img->w / tw) : 1;
            int strip_rows = (img && th > 0) ? (img->h / th) : 1;
            td.static_tile_count = td.strip_cols * strip_rows;
            td.cols = cols;
            td.rows = rows;
            td.cells.assign((size_t)cols * (size_t)rows, 0);
        });

    // setCell / getCell — indices: col 0..cols-1, row 0..rows-1.
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "setCell", "(III)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (!td) return;
            int c = args[1].as_int(), r = args[2].as_int(), v = args[3].as_int();
            if (c < 0 || r < 0 || c >= td->cols || r >= td->rows) return;
            td->cells[(size_t)r * td->cols + c] = v;
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getCell", "(II)I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            int c = args[1].as_int(), r = args[2].as_int();
            if (!td || c < 0 || r < 0 || c >= td->cols || r >= td->rows) {
                f.push_int(0); return;
            }
            f.push_int(td->cells[(size_t)r * td->cols + c]);
        });

    // fillCells(col, row, numCols, numRows, tileIndex)
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "fillCells", "(IIIII)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (!td) return;
            int c0 = args[1].as_int(), r0 = args[2].as_int();
            int nc = args[3].as_int(), nr = args[4].as_int();
            int v  = args[5].as_int();
            for (int r = r0; r < r0 + nr && r < td->rows; ++r) {
                if (r < 0) continue;
                for (int c = c0; c < c0 + nc && c < td->cols; ++c) {
                    if (c < 0) continue;
                    td->cells[(size_t)r * td->cols + c] = v;
                }
            }
        });

    // Animated tiles: createAnimatedTile(staticTile) returns negative id;
    // setAnimatedTile(id, staticTile) updates which static tile the id maps to.
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "createAnimatedTile", "(I)I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (!td) { f.push_int(0); return; }
            td->animated.push_back(args[1].as_int());
            f.push_int(-(int)td->animated.size());  // -1, -2, -3, ...
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "setAnimatedTile", "(II)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (!td) return;
            int id = args[1].as_int();
            int v  = args[2].as_int();
            int idx = -id - 1;
            if (idx >= 0 && idx < (int)td->animated.size())
                td->animated[idx] = v;
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getAnimatedTile", "(I)I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            int id = args[1].as_int();
            int idx = -id - 1;
            if (!td || idx < 0 || idx >= (int)td->animated.size()) {
                f.push_int(0); return;
            }
            f.push_int(td->animated[idx]);
        });

    // Dimensions and counts
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getColumns", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->cols : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getRows", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->rows : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getCellWidth", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->tile_w : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getCellHeight", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->tile_h : 0);
        });

    // Layer base: setPosition / move / getX / getY / getWidth / getHeight /
    // setVisible / isVisible — keyed off the TiledData via its own Layer
    // accessors. We registered these on Layer earlier for Sprite; they need
    // to also work for TiledLayer since Sprite/TiledLayer share Layer methods
    // through inheritance. The existing Layer registrations look up g_sprites,
    // which won't find a TiledLayer ObjRef. Re-register with a multi-map
    // lookup.
    auto layer_x = [tiled_for](ObjRef ref, int* out_x, int* out_y,
                               int* out_w, int* out_h, bool* visible) {
        TiledData* td = tiled_for(ref);
        if (td) {
            if (out_x) *out_x = td->x;
            if (out_y) *out_y = td->y;
            if (out_w) *out_w = td->cols * td->tile_w;
            if (out_h) *out_h = td->rows * td->tile_h;
            if (visible) *visible = td->visible;
            return true;
        }
        return false;
    };
    auto layer_set_pos = [tiled_for](ObjRef ref, int x, int y) {
        TiledData* td = tiled_for(ref);
        if (td) { td->x = x; td->y = y; }
    };
    auto layer_set_visible = [tiled_for](ObjRef ref, bool v) {
        TiledData* td = tiled_for(ref);
        if (td) td->visible = v;
    };

    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "paint", "(Ljavax/microedition/lcdui/Graphics;)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (!td || !td->visible || !td->img) return;
            ObjRef gfx = args[1].as_ref();
            SDL_Surface* dst = gfx_surface(gfx);
            if (!dst) return;
            auto t = gfx_tx(gfx);
            for (int r = 0; r < td->rows; ++r) {
                for (int c = 0; c < td->cols; ++c) {
                    int v = td->cells[(size_t)r * td->cols + c];
                    if (v == 0) continue;
                    if (v < 0) {
                        int idx = -v - 1;
                        if (idx < 0 || idx >= (int)td->animated.size()) continue;
                        v = td->animated[idx];
                        if (v <= 0) continue;
                    }
                    if (v > td->static_tile_count) continue;
                    int tile = v - 1;  // strip is 0-based
                    int sx = (tile % td->strip_cols) * td->tile_w;
                    int sy = (tile / td->strip_cols) * td->tile_h;
                    blit_image(dst, td->img,
                               td->x + c * td->tile_w + t.x,
                               td->y + r * td->tile_h + t.y,
                               sx, sy, td->tile_w, td->tile_h, 0);
                }
            }
        });

    (void)layer_x; (void)layer_set_pos; (void)layer_set_visible;

    // The Layer-base method registrations earlier (setPosition, getX, …) only
    // look up g_sprites; they would no-op for TiledLayer. Re-register the
    // same six methods directly on TiledLayer so virtual dispatch finds the
    // TiledData-aware versions first.
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "setPosition", "(II)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (td) { td->x = args[1].as_int(); td->y = args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "move", "(II)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (td) { td->x += args[1].as_int(); td->y += args[2].as_int(); }
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getX", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->x : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getY", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->y : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getWidth", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->cols * td->tile_w : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "getHeight", "()I",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td ? td->rows * td->tile_h : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "setVisible", "(Z)V",
        [tiled_for](VM&, Frame&, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            if (td) td->visible = args[1].as_int() != 0;
        });
    vm.register_native("javax/microedition/lcdui/game/TiledLayer",
        "isVisible", "()Z",
        [tiled_for](VM&, Frame& f, std::span<Slot> args) {
            TiledData* td = tiled_for(args[0].as_ref());
            f.push_int(td && td->visible ? 1 : 0);
        });

    // ── javax.microedition.lcdui.game.LayerManager ───────────────────────────
    // Container for Layer instances with a viewport. paint(g, x, y) iterates
    // layers in z-order (last-appended draws first per spec) within the
    // current viewport, calling each Layer's paint with the right offset.
    struct LMState {
        std::vector<ObjRef> layers;
        int view_x = 0, view_y = 0, view_w = 0, view_h = 0;
    };
    static std::unordered_map<ObjRef, LMState> g_lms;
    auto lm_for = [](ObjRef r) -> LMState* {
        auto it = g_lms.find(r);
        return it == g_lms.end() ? nullptr : &it->second;
    };

    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_lms[args[0].as_ref()] = {};
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "append", "(Ljavax/microedition/lcdui/game/Layer;)V",
        [lm_for](VM&, Frame&, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            if (lm) lm->layers.push_back(args[1].as_ref());
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "insert", "(Ljavax/microedition/lcdui/game/Layer;I)V",
        [lm_for](VM&, Frame&, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            if (!lm) return;
            int i = args[2].as_int();
            if (i < 0) i = 0;
            if (i > (int)lm->layers.size()) i = (int)lm->layers.size();
            lm->layers.insert(lm->layers.begin() + i, args[1].as_ref());
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "remove", "(Ljavax/microedition/lcdui/game/Layer;)V",
        [lm_for](VM&, Frame&, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            if (!lm) return;
            ObjRef target = args[1].as_ref();
            auto it = std::find(lm->layers.begin(), lm->layers.end(), target);
            if (it != lm->layers.end()) lm->layers.erase(it);
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "getLayerAt", "(I)Ljavax/microedition/lcdui/game/Layer;",
        [lm_for](VM&, Frame& f, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            int i = args[1].as_int();
            f.push_ref((lm && i >= 0 && i < (int)lm->layers.size())
                       ? lm->layers[i] : NULL_REF);
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "getSize", "()I",
        [lm_for](VM&, Frame& f, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            f.push_int(lm ? (int)lm->layers.size() : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "setViewWindow", "(IIII)V",
        [lm_for](VM&, Frame&, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            if (!lm) return;
            lm->view_x = args[1].as_int();
            lm->view_y = args[2].as_int();
            lm->view_w = args[3].as_int();
            lm->view_h = args[4].as_int();
        });
    // paint(Graphics, int x, int y): paint all layers in z-order (highest
    // index = back-most per spec; we iterate in reverse so the first-added
    // ends up on top).
    vm.register_native("javax/microedition/lcdui/game/LayerManager",
        "paint", "(Ljavax/microedition/lcdui/Graphics;II)V",
        [lm_for, sprite_for, tiled_for](VM& v, Frame&, std::span<Slot> args) {
            LMState* lm = lm_for(args[0].as_ref());
            if (!lm || lm->layers.empty()) return;
            ObjRef gfx = args[1].as_ref();
            SDL_Surface* dst = gfx_surface(gfx);
            if (!dst) return;
            int paint_x = args[2].as_int(), paint_y = args[3].as_int();
            // Iterate back-to-front (last-appended on top per spec)
            for (auto it = lm->layers.rbegin(); it != lm->layers.rend(); ++it) {
                ObjRef layer = *it;
                // Sprite
                if (SpriteData* sd = sprite_for(layer)) {
                    if (!sd->visible || !sd->img) continue;
                    int raw = (sd->frame >= 0 && sd->frame < (int)sd->seq.size())
                                ? sd->seq[sd->frame] : 0;
                    if (raw < 0 || raw >= sd->frame_count) raw = 0;
                    int sx = (raw % sd->cols) * sd->frame_w;
                    int sy = (raw / sd->cols) * sd->frame_h;
                    auto t = gfx_tx(gfx);
                    blit_image(dst, sd->img,
                               paint_x + sd->x - lm->view_x + t.x,
                               paint_y + sd->y - lm->view_y + t.y,
                               sx, sy, sd->frame_w, sd->frame_h, sd->transform);
                    continue;
                }
                // TiledLayer
                if (TiledData* td = tiled_for(layer)) {
                    if (!td->visible || !td->img) continue;
                    auto t = gfx_tx(gfx);
                    for (int r = 0; r < td->rows; ++r) {
                        for (int c = 0; c < td->cols; ++c) {
                            int v = td->cells[(size_t)r * td->cols + c];
                            if (v == 0) continue;
                            if (v < 0) {
                                int idx = -v - 1;
                                if (idx < 0 || idx >= (int)td->animated.size()) continue;
                                v = td->animated[idx];
                                if (v <= 0) continue;
                            }
                            if (v > td->static_tile_count) continue;
                            int tile = v - 1;
                            int sx = (tile % td->strip_cols) * td->tile_w;
                            int sy = (tile / td->strip_cols) * td->tile_h;
                            blit_image(dst, td->img,
                                       paint_x + td->x + c * td->tile_w - lm->view_x + t.x,
                                       paint_y + td->y + r * td->tile_h - lm->view_y + t.y,
                                       sx, sy, td->tile_w, td->tile_h, 0);
                        }
                    }
                    continue;
                }
                // Custom Layer subclass — invoke its paint(Graphics)
                HeapObject* ho = v.heap().deref(layer);
                if (!ho || !ho->klass) continue;
                if (auto* m = ho->klass->resolve_virtual("paint",
                        "(Ljavax/microedition/lcdui/Graphics;)V")) {
                    Slot args2[2] = { Slot::from_ref(layer), Slot::from_ref(gfx) };
                    v.invoke(m, ho->klass, std::span<const Slot>(args2, 2));
                }
            }
        });

    // ── Sprite.collidesWith — bounding-box variants ──────────────────────────
    auto sprite_bbox = [sprite_for](ObjRef ref, int& x, int& y, int& w, int& h) {
        SpriteData* sd = sprite_for(ref);
        if (!sd) return false;
        x = sd->x; y = sd->y;
        if (sd->transform == 4 || sd->transform == 5 ||
            sd->transform == 6 || sd->transform == 7) {
            w = sd->frame_h; h = sd->frame_w;
        } else {
            w = sd->frame_w; h = sd->frame_h;
        }
        return true;
    };
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "collidesWith", "(Ljavax/microedition/lcdui/game/Sprite;Z)Z",
        [sprite_bbox](VM&, Frame& f, std::span<Slot> args) {
            int ax, ay, aw, ah, bx, by, bw, bh;
            if (!sprite_bbox(args[0].as_ref(), ax, ay, aw, ah) ||
                !sprite_bbox(args[1].as_ref(), bx, by, bw, bh)) {
                f.push_int(0); return;
            }
            // bool pixelLevel = args[2].as_int() != 0;
            // Always do bbox; pixel-level upgrade can come later.
            bool overlap = ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
            f.push_int(overlap ? 1 : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "collidesWith",
        "(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z",
        [sprite_bbox, tiled_for](VM&, Frame& f, std::span<Slot> args) {
            int ax, ay, aw, ah;
            if (!sprite_bbox(args[0].as_ref(), ax, ay, aw, ah)) {
                f.push_int(0); return;
            }
            TiledData* td = tiled_for(args[1].as_ref());
            if (!td) { f.push_int(0); return; }
            int tw = td->cols * td->tile_w, th = td->rows * td->tile_h;
            bool overlap = ax < td->x+tw && ax+aw > td->x &&
                           ay < td->y+th && ay+ah > td->y;
            f.push_int(overlap ? 1 : 0);
        });
    vm.register_native("javax/microedition/lcdui/game/Sprite",
        "collidesWith",
        "(Ljavax/microedition/lcdui/Image;IIZ)Z",
        [sprite_bbox](VM& v, Frame& f, std::span<Slot> args) {
            int ax, ay, aw, ah;
            if (!sprite_bbox(args[0].as_ref(), ax, ay, aw, ah)) {
                f.push_int(0); return;
            }
            ObjRef img_ref = args[1].as_ref();
            int ix = args[2].as_int(), iy = args[3].as_int();
            int iw = 0, ih = 0;
            auto img_it = g_images.find(img_ref);
            if (img_it != g_images.end() && img_it->second) {
                iw = img_it->second->w; ih = img_it->second->h;
            }
            (void)v;
            bool overlap = ax < ix+iw && ax+aw > ix && ay < iy+ih && ay+ah > iy;
            f.push_int(overlap ? 1 : 0);
        });
}
