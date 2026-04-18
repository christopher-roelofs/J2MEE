#include "natives.hpp"
#include "backend/display.hpp"
#include "vm/vm.hpp"
#include "vm/heap.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <cstring>
#include <cmath>
#include <unordered_map>

// ─── External state ──────────────────────────────────────────────────────────
// Graphics objects draw into SDL surfaces.  The "main" graphics always draws
// into Display::instance().screen().  Off-screen Image surfaces are kept here.

// Declared in natives.cpp — shared with List UI
extern std::unordered_map<ObjRef, ObjRef> g_command_listeners;

// Commands added to each Displayable (first=left soft key, rest=right soft key menu)
static std::unordered_map<ObjRef, std::vector<ObjRef>> g_commands;

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

static constexpr const char* FONT_PATH       = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
static constexpr const char* FONT_BOLD_PATH   = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
static constexpr const char* FONT_MONO_PATH   = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
static constexpr const char* FONT_MONO_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
static constexpr const char* FONT_CJK_PATH   = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";

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

        // Step 1: Extract the source region into its own surface
        SDL_Surface* region = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!region) return;
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);  // copy raw pixels
        SDL_Rect srect{sx, sy, sw, sh};
        SDL_BlitSurface(src, &srect, region, nullptr);
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_BLEND); // restore

        // Step 2: Transform pixels into dest-sized surface
        SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!tmp) { SDL_FreeSurface(region); return; }
        SDL_FillRect(tmp, nullptr, 0x00000000u);

        SDL_LockSurface(region);
        SDL_LockSurface(tmp);
        for (int r = 0; r < sh; ++r) {
            for (int c = 0; c < sw; ++c) {
                uint32_t pixel;
                std::memcpy(&pixel,
                    (uint8_t*)region->pixels + r * region->pitch + c * 4, 4);
                if ((pixel >> 24) == 0) continue;

                int dr = r, dc = c;
                switch (transform) {
                    case 1: dr = sh-1-r; dc = c;        break; // MIRROR_ROT180 (vertical flip)
                    case 2: dr = r;      dc = sw-1-c;   break; // MIRROR (horizontal flip)
                    case 3: dr = sh-1-r; dc = sw-1-c;   break; // ROT180
                    case 4: dr = sw-1-c; dc = sh-1-r;   break; // MIRROR_ROT270
                    case 5: dr = c;      dc = sh-1-r;   break; // ROT90 (clockwise)
                    case 6: dr = sw-1-c; dc = r;        break; // ROT270 (counter-clockwise)
                    case 7: dr = c;      dc = r;        break; // MIRROR_ROT90
                }
                if (dc < 0 || dc >= dw || dr < 0 || dr >= dh) continue;
                std::memcpy(
                    (uint8_t*)tmp->pixels + dr * tmp->pitch + dc * 4,
                    &pixel, 4);
            }
        }
        SDL_UnlockSurface(tmp);
        SDL_UnlockSurface(region);
        SDL_FreeSurface(region);
        SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);
        SDL_Rect drect{dx, dy, dw, dh};
        SDL_BlitSurface(tmp, nullptr, dst, &drect);
        SDL_FreeSurface(tmp);
    }
}

// ─── Image loading ────────────────────────────────────────────────────────────

// Load PNG bytes → SDL_Surface (ARGB8888)
static SDL_Surface* load_png_from_bytes(const uint8_t* data, size_t len) {
    SDL_RWops* rw = SDL_RWFromConstMem(data, static_cast<int>(len));
    if (!rw) return nullptr;
    SDL_Surface* raw = IMG_Load_RW(rw, 1);  // 1 = auto-close
    if (!raw) return nullptr;
    // Preserve color key transparency through format conversion
    uint32_t ckey = 0;
    bool has_colorkey = (SDL_GetColorKey(raw, &ckey) == 0);
    bool has_alpha = (raw->format->Amask != 0);

    // Convert color key to RGBA values we can work with
    uint8_t ck_r = 0, ck_g = 0, ck_b = 0;
    if (has_colorkey)
        SDL_GetRGB(ckey, raw->format, &ck_r, &ck_g, &ck_b);

    SDL_Surface* converted = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!converted) return nullptr;

    SDL_LockSurface(converted);
    uint32_t* px = static_cast<uint32_t*>(converted->pixels);
    int count = converted->w * converted->h;
    if (has_colorkey) {
        // Make color key pixels transparent, everything else opaque
        uint32_t ck_rgb = ((uint32_t)ck_r << 16) | ((uint32_t)ck_g << 8) | ck_b;
        for (int i = 0; i < count; i++) {
            if ((px[i] & 0x00FFFFFFu) == ck_rgb)
                px[i] = 0x00000000u;  // transparent
            else
                px[i] |= 0xFF000000u;  // opaque
        }
    } else if (!has_alpha) {
        // No alpha, no color key — make all pixels fully opaque
        for (int i = 0; i < count; i++)
            px[i] |= 0xFF000000u;
    }
    SDL_UnlockSurface(converted);
    SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_BLEND);
    return converted;
}

// ─── Registration ─────────────────────────────────────────────────────────────

int g_screen_w = 240;
int g_screen_h = 320;

void register_graphics_natives(VM& vm, const JarFile& jar) {

    // Try to read screen resolution from boxal.inf key 21 (w,h)
    if (jar.has("boxal.inf")) {
        auto& data = jar.get("boxal.inf");
        std::string inf(data.begin(), data.end());
        // Parse line by line looking for "21,W,H"
        size_t pos = 0;
        while (pos < inf.size()) {
            size_t eol = inf.find_first_of("\r\n", pos);
            if (eol == std::string::npos) eol = inf.size();
            std::string line = inf.substr(pos, eol - pos);
            if (line.substr(0, 3) == "21,") {
                int w = 0, h = 0;
                if (sscanf(line.c_str(), "21,%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                    g_screen_w = w;
                    g_screen_h = h;
                    fprintf(stderr, "[display] resolution from boxal.inf: %dx%d\n", w, h);
                }
            }
            pos = eol;
            while (pos < inf.size() && (inf[pos] == '\r' || inf[pos] == '\n')) ++pos;
        }
    }

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
    vm.register_native("javax/microedition/lcdui/Graphics",
        "setStrokeStyle", "(I)V",
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

    vm.register_native("javax/microedition/lcdui/Canvas",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Display::instance().width());
        });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Display::instance().height());
        });
    vm.register_native("javax/microedition/lcdui/Displayable",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Display::instance().width());
        });
    vm.register_native("javax/microedition/lcdui/Displayable",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Display::instance().height());
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

    vm.register_native("javax/microedition/lcdui/Canvas",
        "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Nokia FullCanvas ─────────────────────────────────────────────────────
    // FullCanvas extends Canvas with full screen mode. Link the class hierarchy
    // so that subclasses of FullCanvas inherit Canvas methods.
    {
        ClassDef* fullCanvas = vm.loader().find_or_stub("com/nokia/mid/ui/FullCanvas");
        ClassDef* canvas = vm.loader().find_or_stub("javax/microedition/lcdui/Canvas");
        if (!fullCanvas->super) fullCanvas->super = canvas;
    }
    vm.register_native("com/nokia/mid/ui/FullCanvas",
        "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // Nokia Sound stub
    vm.register_native("com/nokia/mid/sound/Sound",
        "<init>", "([BI)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("com/nokia/mid/sound/Sound",
        "play", "(I)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("com/nokia/mid/sound/Sound",
        "stop", "()V",
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
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Timer / TimerTask ────────────────────────────────────────────────────
    // J2ME Timer.schedule(task, delay, period) runs task.run() repeatedly.
    // We implement this as a loop inside the deferred thread system.

    vm.register_native("java/util/TimerTask", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_native("java/util/Timer", "<init>", "()V",
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
            if (ms <= 0) ms = 1;  // 0 means "yield" in our model
            // Sleep on the green-thread scheduler, not SDL_Delay. This
            // blocks only THIS thread — other Java threads get to run.
            // The scheduler's main loop polls SDL events between dispatches
            // so keyboard/window-close still work while we're here.
            v.scheduler.sleep_current((uint64_t)ms);
            if (!Display::instance().flush())
                throw QuitRequest{};
        });
}
