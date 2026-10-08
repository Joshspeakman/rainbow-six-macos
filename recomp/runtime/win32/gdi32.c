/* GDI32: device contexts, memory bitmaps and TrueType text through Core Text (the game rasterises font glyphs
 * with ExtTextOutA and reads them back with GetPixel/GetDIBits to build font textures). */
#include "w32.h"
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
typedef CTFontRef HostFont;
#else
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
typedef FT_Face HostFont;
#endif

extern int w32_screen_w, w32_screen_h;
enum { G_DC = 1, G_BITMAP, G_FONT, G_BRUSH, G_PEN, G_STOCK };
typedef struct GObj {
    int type;
    /* bitmap */ int bw, bh; uint32_t *px;            /* 0x00RRGGBB per pixel, top-down */
    /* font */   HostFont font; int height, weight, italic; char face[64]; int ascent, descent, internal_leading, avg_w, max_w;
    /* dc */     uint32_t bitmap, fontsel, text_color, bk_color; int bk_mode;
    uint32_t color;
} GObj;
#define MAXG 1024
#define GBASE 0x00b00000u
static GObj *gtab[MAXG]; static pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_new(GObj *o) { pthread_mutex_lock(&glock); for (int k = 1; k < MAXG; k++) if (!gtab[k]) { gtab[k] = o; pthread_mutex_unlock(&glock); return GBASE + 4 * (uint32_t)k; } pthread_mutex_unlock(&glock); return 0; }
static GObj *G(uint32_t h, int type) { uint32_t k = (h - GBASE) / 4; GObj *o = (h >= GBASE && k < MAXG) ? gtab[k] : 0; return o && (!type || o->type == type) ? o : 0; }
static uint32_t screen_dc;
static GObj *dc_of(uint32_t h)
{
    if (h == 0x20004) {                                   /* GetDC(window): a screen-compatible DC */
        if (!screen_dc) { GObj *d = calloc(1, sizeof *d); d->type = G_DC; d->text_color = 0; d->bk_color = 0xffffff; d->bk_mode = 2; screen_dc = g_new(d); }
        return G(screen_dc, G_DC);
    }
    return G(h, G_DC);
}

IMPL(gdi32, GetStockObject) { RET(0x00b80000u + (ARG(0) & 0xff), 1); }
IMPL(gdi32, CreateHatchBrush) { GObj *o = calloc(1, sizeof *o); o->type = G_BRUSH; o->color = ARG(1); RET(g_new(o), 2); }
IMPL(gdi32, CreateCompatibleDC)
{
    GObj *o = calloc(1, sizeof *o); o->type = G_DC; o->text_color = 0; o->bk_color = 0xffffff; o->bk_mode = 2;
    RET(g_new(o), 1);
}
IMPL(gdi32, DeleteDC) { uint32_t k = (ARG(0) - GBASE) / 4; GObj *o = G(ARG(0), G_DC); if (o) { free(o); gtab[k] = 0; } RET(o != 0, 1); }
IMPL(gdi32, CreateCompatibleBitmap)
{
    int w = (int)ARG(1), h = (int)ARG(2); if (w <= 0 || h <= 0 || w > 8192 || h > 8192) RET(0, 3);
    GObj *o = calloc(1, sizeof *o); o->type = G_BITMAP; o->bw = w; o->bh = h; o->px = calloc((size_t)w * h, 4);
    RET(g_new(o), 3);
}
IMPL(gdi32, DeleteObject)
{
    uint32_t h = ARG(0), k = (h - GBASE) / 4; GObj *o = G(h, 0);
    if (!o) RET(1, 1);
    if (o->type == G_BITMAP) free(o->px);
    if (o->type == G_FONT && o->font) {
#ifdef __APPLE__
        CFRelease(o->font);
#else
        FT_Done_Face(o->font);
#endif
    }
    free(o); gtab[k] = 0; RET(1, 1);
}
IMPL(gdi32, SelectObject)
{
    GObj *d = dc_of(ARG(0)); uint32_t h = ARG(1), old = 0; GObj *o = G(h, 0);
    if (!d) RET(0, 2);
    if (o && o->type == G_BITMAP) { old = d->bitmap; d->bitmap = h; }
    else if (o && o->type == G_FONT) { old = d->fontsel; d->fontsel = h; }
    else old = 0x00b80000u;                                 /* brushes, pens, stock objects: tracked loosely */
    RET(old ? old : 0x00b80000u, 2);
}
IMPL(gdi32, SetBkMode) { GObj *d = dc_of(ARG(0)); int o = d ? d->bk_mode : 0; if (d) d->bk_mode = (int)ARG(1); RET((uint32_t)o, 2); }
IMPL(gdi32, SetTextColor) { GObj *d = dc_of(ARG(0)); uint32_t o = d ? d->text_color : 0; if (d) d->text_color = ARG(1); RET(o, 2); }
IMPL(gdi32, SetBkColor) { GObj *d = dc_of(ARG(0)); uint32_t o = d ? d->bk_color : 0; if (d) d->bk_color = ARG(1); RET(o, 2); }
IMPL(gdi32, GetDeviceCaps)
{
    switch (ARG(1)) {
    case 8: RET((uint32_t)w32_screen_w, 2); case 10: RET((uint32_t)w32_screen_h, 2);
    case 12: RET(32, 2); case 14: RET(1, 2); case 24: RET(0xffffffffu, 2);
    case 88: case 90: RET(96, 2); case 116: RET(60, 2); case 38: RET(0x7e99, 2);
    case 4: RET(320, 2); case 6: RET(240, 2);
    }
    RET(0, 2);
}
IMPL(gdi32, DPtoLP) { RET(1, 3); }

/* ---- fonts ---- */
#ifdef __APPLE__
static CFStringRef mac_face(const char *face)
{
    static const char *map[][2] = {{"ms sans serif", "Arial"}, {"ms serif", "Times New Roman"}, {"system", "Arial"},
        {"fixedsys", "Courier New"}, {"terminal", "Courier New"}, {"small fonts", "Arial"}, {"ms shell dlg", "Arial"}, {0, 0}};
    for (int k = 0; map[k][0]; k++) if (!strcasecmp(face, map[k][0])) face = map[k][1];
    if (!*face) face = "Arial";
    CFStringRef r = CFStringCreateWithCString(0, face, kCFStringEncodingWindowsLatin1);
    return r ? r : CFStringCreateWithCString(0, "Arial", kCFStringEncodingASCII);   /* a name that doesn't convert */
}
static uint32_t make_font(int height, int weight, int italic, const char *face)
{
    GObj *o = calloc(1, sizeof *o); o->type = G_FONT; o->height = height; o->weight = weight; o->italic = italic;
    snprintf(o->face, sizeof o->face, "%s", face);
    CFStringRef name = mac_face(face);
    CTFontRef f = CTFontCreateWithName(name, 100, 0); CFRelease(name);
    CTFontSymbolicTraits tr = (weight >= 600 ? kCTFontBoldTrait : 0) | (italic ? kCTFontItalicTrait : 0);
    if (tr) { CTFontRef g = CTFontCreateCopyWithSymbolicTraits(f, 100, 0, tr, tr); if (g) { CFRelease(f); f = g; } }
    double asc = CTFontGetAscent(f), desc = CTFontGetDescent(f);
    double size = height < 0 ? -height : height > 0 ? height * 100.0 / (asc + desc) : 12;   /* Windows: <0 em height, >0 cell height */
    o->font = CTFontCreateCopyWithAttributes(f, size, 0, 0); CFRelease(f);
    o->ascent = (int)ceil(CTFontGetAscent(o->font)); o->descent = (int)ceil(CTFontGetDescent(o->font));
    o->internal_leading = o->ascent + o->descent - (int)lround(size);
    if (o->internal_leading < 0) o->internal_leading = 0;
    UniChar ch[2] = {'x', 'W'}; CGGlyph gl[2]; CGSize adv[2];
    CTFontGetGlyphsForCharacters(o->font, ch, gl, 2); CTFontGetAdvancesForGlyphs(o->font, kCTFontOrientationHorizontal, gl, adv, 2);
    o->avg_w = (int)lround(adv[0].width); o->max_w = (int)lround(adv[1].width);
    return g_new(o);
}
#else
static FT_Library ft_library;
static pthread_once_t font_once = PTHREAD_ONCE_INIT;
static void init_fonts(void) { FT_Init_FreeType(&ft_library); FcInit(); }
static uint32_t make_font(int height, int weight, int italic, const char *face) {
    pthread_once(&font_once, init_fonts); if (!ft_library) return 0;
    GObj *o = calloc(1, sizeof *o); if (!o) return 0;
    o->type = G_FONT; o->height = height; o->weight = weight; o->italic = italic;
    snprintf(o->face, sizeof o->face, "%s", face);
    const char *family = *face ? face : "Arial";
    if (!strcasecmp(face, "ms sans serif") || !strcasecmp(face, "system") || !strcasecmp(face, "ms shell dlg")) family = "sans-serif";
    if (!strcasecmp(face, "fixedsys") || !strcasecmp(face, "terminal")) family = "monospace";
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pattern, FC_WEIGHT, weight >= 600 ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern); FcDefaultSubstitute(pattern);
    FcResult result; FcPattern *match = FcFontMatch(NULL, pattern, &result); FcChar8 *file = NULL; int index = 0;
    int ok = match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch;
    if (ok) { FcPatternGetInteger(match, FC_INDEX, 0, &index); ok = !FT_New_Face(ft_library, (const char *)file, index, &o->font); }
    if (match) FcPatternDestroy(match); FcPatternDestroy(pattern);
    if (!ok) { free(o); return 0; }
    double size = height < 0 ? -height : height > 0 ? (double)height * o->font->units_per_EM / (o->font->ascender - o->font->descender) : 12;
    if (size < 1) size = 1;
    FT_Set_Char_Size(o->font, 0, (FT_F26Dot6)lround(size * 64), 72, 72);
    o->ascent = (int)((o->font->size->metrics.ascender + 63) >> 6);
    o->descent = (int)((-o->font->size->metrics.descender + 63) >> 6);
    o->internal_leading = o->ascent + o->descent - (int)lround(size); if (o->internal_leading < 0) o->internal_leading = 0;
    FT_Load_Char(o->font, 'x', FT_LOAD_DEFAULT); o->avg_w = (int)((o->font->glyph->advance.x + 32) >> 6);
    FT_Load_Char(o->font, 'W', FT_LOAD_DEFAULT); o->max_w = (int)((o->font->glyph->advance.x + 32) >> 6);
    return g_new(o);
}
#endif
IMPL(gdi32, CreateFontA) { RET(make_font((int)ARG(0), (int)ARG(4), (int)ARG(5), ARG(13) ? GS(ARG(13)) : ""), 14); }
IMPL(gdi32, CreateFontIndirectA)
{
    uint32_t lf = ARG(0); char face[33]; memcpy(face, GP(lf + 28), 32); face[32] = 0;
    RET(make_font((int)rt_r32(G_MEM, lf), (int)rt_r32(G_MEM, lf + 16), rt_r8(G_MEM, lf + 20), face), 1);
}
static GObj *dc_font(GObj *d) { GObj *f = d ? G(d->fontsel, G_FONT) : 0; if (!f && d) { d->fontsel = make_font(-13, 400, 0, "Arial"); f = G(d->fontsel, G_FONT); } return f; }
IMPL(gdi32, GetTextMetricsA)
{
    GObj *f = dc_font(dc_of(ARG(0))); uint32_t t = ARG(1); if (!f) RET(0, 2);
    memset(GP(t), 0, 56);
    uint32_t v[11] = {(uint32_t)(f->ascent + f->descent), (uint32_t)f->ascent, (uint32_t)f->descent, (uint32_t)f->internal_leading, 0,
                      (uint32_t)f->avg_w, (uint32_t)f->max_w, (uint32_t)f->weight, 0, 96, 96};
    memcpy(GP(t), v, sizeof v);
    rt_w8(G_MEM, t + 44, 32); rt_w8(G_MEM, t + 45, 255); rt_w8(G_MEM, t + 46, '?'); rt_w8(G_MEM, t + 47, ' ');
    rt_w8(G_MEM, t + 48, (uint8_t)f->italic); rt_w8(G_MEM, t + 51, 0x06);   /* TMPF_VECTOR | TMPF_TRUETYPE */
    RET(1, 2);
}
#ifdef __APPLE__
static double advance(GObj *f, uint16_t ch)
{
    UniChar u = ch; CGGlyph g; CGSize a = {0, 0};
    if (CTFontGetGlyphsForCharacters(f->font, &u, &g, 1)) CTFontGetAdvancesForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &a, 1);
    return a.width;
}
#else
static double advance(GObj *f, uint16_t ch) {
    if (FT_Load_Char(f->font, ch, FT_LOAD_DEFAULT)) return 0;
    return f->font->glyph->advance.x / 64.0;
}
#endif
IMPL(gdi32, GetCharWidthA)
{
    GObj *f = dc_font(dc_of(ARG(0))); uint32_t first = ARG(1), last = ARG(2), out = ARG(3); if (!f) RET(0, 4);
    for (uint32_t ch = first; ch <= last; ch++) {
        uint16_t w; uint8_t b = (uint8_t)ch; w32_mb_to_wide(1252, &b, 1, &w, 1);
        rt_w32(G_MEM, out + 4 * (ch - first), (uint32_t)lround(advance(f, w)));
    }
    RET(1, 4);
}
/* draw UTF-16 text at (x, y) = top-left of the text cell, in the DC's text colour (and background when OPAQUE) */
#ifdef __APPLE__
static void draw_text(GObj *d, int x, int y, const uint16_t *s, int n)
{
    GObj *bm = G(d->bitmap, G_BITMAP), *f = dc_font(d);
    if (!bm || !f || n <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(bm->px, (size_t)bm->bw, (size_t)bm->bh, 8, (size_t)bm->bw * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    CGContextTranslateCTM(cg, 0, bm->bh); CGContextScaleCTM(cg, 1, -1);                 /* top-down like GDI */
    CGContextSetShouldSmoothFonts(cg, false);
    uint32_t tc = d->text_color;
    CGFloat comps[4] = {(tc & 0xff) / 255.0, ((tc >> 8) & 0xff) / 255.0, ((tc >> 16) & 0xff) / 255.0, 1};
    CFStringRef str = CFStringCreateWithCharacters(0, s, n);
    CGColorRef col = CGColorCreateGenericRGB(comps[0], comps[1], comps[2], 1);
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName}; const void *vals[] = {f->font, col};
    CFDictionaryRef attrs = CFDictionaryCreate(0, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(0, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    if (d->bk_mode == 2) {                                                               /* OPAQUE */
        uint32_t bc = d->bk_color; double w = CTLineGetTypographicBounds(line, 0, 0, 0);
        CGContextSetRGBFillColor(cg, (bc & 0xff) / 255.0, ((bc >> 8) & 0xff) / 255.0, ((bc >> 16) & 0xff) / 255.0, 1);
        CGContextFillRect(cg, CGRectMake(x, y, w, f->ascent + f->descent));
    }
    CGContextSetTextMatrix(cg, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(cg, x, y + f->ascent);
    CTLineDraw(line, cg);
    CFRelease(line); CFRelease(as); CFRelease(attrs); CGColorRelease(col); CFRelease(str); CGContextRelease(cg);
}
#else
static uint32_t rgb_from_colorref(uint32_t c) { return ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255); }
static void draw_text(GObj *d, int x, int y, const uint16_t *s, int n) {
    GObj *bm = G(d->bitmap, G_BITMAP), *f = dc_font(d); if (!bm || !f || n <= 0) return;
    double width = 0; for (int i = 0; i < n; ++i) width += advance(f, s[i]);
    uint32_t fg = rgb_from_colorref(d->text_color), bg = rgb_from_colorref(d->bk_color);
    if (d->bk_mode == 2) for (int yy = y; yy < y + f->ascent + f->descent; ++yy)
        for (int xx = x; xx < x + (int)ceil(width); ++xx) if (xx >= 0 && xx < bm->bw && yy >= 0 && yy < bm->bh) bm->px[yy * bm->bw + xx] = bg;
    double pen = x;
    for (int i = 0; i < n; ++i) {
        if (FT_Load_Char(f->font, s[i], FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
        FT_GlyphSlot g = f->font->glyph; FT_Bitmap *b = &g->bitmap;
        for (unsigned row = 0; row < b->rows; ++row) for (unsigned col = 0; col < b->width; ++col) {
            int xx = (int)lround(pen) + g->bitmap_left + (int)col, yy = y + f->ascent - g->bitmap_top + (int)row;
            if (xx < 0 || xx >= bm->bw || yy < 0 || yy >= bm->bh) continue;
            const unsigned char *src = b->buffer + (b->pitch >= 0 ? row : b->rows - 1 - row) * abs(b->pitch);
            unsigned a = b->pixel_mode == FT_PIXEL_MODE_MONO ? ((src[col / 8] >> (7 - col % 8)) & 1) * 255 : src[col];
            uint32_t old = bm->px[yy * bm->bw + xx], color = 0;
            for (int shift = 0; shift <= 16; shift += 8) color |= ((((fg >> shift) & 255) * a + ((old >> shift) & 255) * (255 - a) + 127) / 255) << shift;
            bm->px[yy * bm->bw + xx] = color;
        }
        pen += g->advance.x / 64.0;
    }
}
#endif
IMPL(gdi32, ExtTextOutA)
{
    GObj *d = dc_of(ARG(0)); uint32_t s = ARG(5); int n = (int)ARG(6);
    if (d && s && n > 0) { uint16_t w[1024]; if (n > 1024) n = 1024; w32_mb_to_wide(1252, (const uint8_t *)GP(s), n, w, n); draw_text(d, (int)ARG(1), (int)ARG(2), w, n); }
    RET(1, 8);
}
IMPL(gdi32, TextOutW)
{
    GObj *d = dc_of(ARG(0)); int n = (int)ARG(4);
    if (d && n > 0) draw_text(d, (int)ARG(1), (int)ARG(2), GW(ARG(3)), n);
    RET(1, 5);
}
IMPL(gdi32, GetPixel)
{
    GObj *d = dc_of(ARG(0)); GObj *bm = d ? G(d->bitmap, G_BITMAP) : 0; int x = (int)ARG(1), y = (int)ARG(2);
    if (!bm || x < 0 || y < 0 || x >= bm->bw || y >= bm->bh) RET(0xffffffffu, 3);
    uint32_t p = bm->px[(size_t)y * bm->bw + x];                                         /* BGRX in memory */
    RET(((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16), 3);                      /* COLORREF 0x00BBGGRR */
}
IMPL(gdi32, GetDIBits)
{
    GObj *bm = G(ARG(1), G_BITMAP); uint32_t start = ARG(2), lines = ARG(3), bits = ARG(4), bi = ARG(5);
    if (!bm) RET(0, 7);
    int32_t h = (int32_t)rt_r32(G_MEM, bi + 8); uint16_t bpp = (uint16_t)rt_r16(G_MEM, bi + 14);
    if (!bits) {                                                                          /* fill in the header */
        rt_w32(G_MEM, bi + 4, (uint32_t)bm->bw); rt_w32(G_MEM, bi + 8, (uint32_t)bm->bh); rt_w16(G_MEM, bi + 12, 1);
        rt_w16(G_MEM, bi + 14, 32); rt_w32(G_MEM, bi + 16, 0); rt_w32(G_MEM, bi + 20, (uint32_t)(bm->bw * bm->bh * 4));
        RET((uint32_t)bm->bh, 7);
    }
    if (bpp != 32 && bpp != 24) { fprintf(stderr, "w32: GetDIBits %u bpp not supported\n", bpp); RET(0, 7); }
    int stride = ((bm->bw * (bpp / 8)) + 3) & ~3, topdown = h < 0;
    for (uint32_t k = 0; k < lines && start + k < (uint32_t)bm->bh; k++) {
        int row = topdown ? (int)(start + k) : bm->bh - 1 - (int)(start + k);
        uint8_t *dst = (uint8_t *)GP(bits + k * (uint32_t)stride); const uint32_t *src = bm->px + (size_t)row * bm->bw;
        for (int x = 0; x < bm->bw; x++) {
            if (bpp == 32) memcpy(dst + 4 * x, &src[x], 4);
            else { dst[3 * x] = (uint8_t)src[x]; dst[3 * x + 1] = (uint8_t)(src[x] >> 8); dst[3 * x + 2] = (uint8_t)(src[x] >> 16); }
        }
    }
    RET(lines, 7);
}
