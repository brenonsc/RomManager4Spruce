// rmui - RomManager's screen engine for spruce's 64-bit handhelds (TrimUI Brick / Brick Pro,
// or any Linux framebuffer). launch.sh describes each screen as lines of text; rmui draws
// it straight to the framebuffer, animates it by itself, and uses no CPU while nothing moves.
//   rmui -d FIFO                          stays running, reading frames from FIFO
//   rmui FRAME                            draws one frame file and exits
//   rmui --thumb IN OUT W H               shrinks a picture to fit W x H, saved as JPEG
//   rmui --preview W H OUT.png FRAME...   (any computer) draws the frames, saves the screen
// Environment: RMUI_FB (default /dev/fb0), RMUI_NOFLIP=1 (no page flipping), RMUI_NOTHREAD=1
// (decode pictures on the main thread), RMUI_PREVIEW=out.png (-d without a screen: each
// frame is saved there instead).
//
// A frame is a list of lines ending with END:
//   THEME_BG / THEME_TEXT / THEME_HL / THEME_HL_TEXT / THEME_DIM:0xAARRGGBB   FONT:path:size
//   BACKGROUND:path   TITLE:text   MOTION:0|2   PERF:1   TRANSITION:push|pop|fade
//   STATUSBAR:text with {WIFI} {NOWIFI} {BATT:n} {DL}   STATUS:text   FOOTER:A/ Play  B/ Back
//   ITEM:text ({CHECK} first, {BAR:n/10} anywhere)   HIGHLIGHT:n   LISTPOS:start:key-total
//   SCROLLBAR:pct   ART:path|NULL|BLANK   PROGRESS:pct   BUSY:text
//   OSK:x:y:text   OSK_KEYS:42 keys   OSK_HINT:text
//   COVERFLOW:sel:total:key   COVER:i:path|BLANK|ICON:name   CFLABEL:text
//   DIALOG:title   DIALOG_LINE:text   DIALOG_BTN:label   DIALOG_SEL:n
//   TILE_ICONS:dir   TILE:icon|title|subtitle ({DL})   TILE_SEL:n   TILE_PRESS:1
//   BOOT:version   BOOT_STEP / BOOT_PROGRESS / BOOT_INFO / BOOT_TIP / BOOT_CREDIT / BOOT_THANKS
// Acting at once: TOAST:ok|err|info:text   LETTER:x   FLUSH_CACHE   QUIT
// /tmp/rm_dlprog ("percent|downloads left") feeds {DL}.
// Everything is laid out on a 640x480 grid (the Miyoo Mini's screen) and scaled to fit.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, a, b) ((v) < (a) ? (a) : (v) > (b) ? (b) : (v))

// ---------------------------------------------------------------- screen and canvas

static int W, H;          // screen, in pixels
static float S = 1;       // 640x480 grid -> screen
static int OX, OY;        // where the grid starts on screen
static uint32_t *canvas;  // what is being drawn (0xAARRGGBB, always opaque)
static uint32_t *prevbuf, *outbuf, *shown;
static int cx0, cy0, cx1, cy1; // clip rectangle (end exclusive)
static int g_alpha = 256;      // fades whole elements (0-256)

static int X(float v) { return OX + (int)lroundf(v * S); }
static int Y(float v) { return OY + (int)lroundf(v * S); }
static int L(float v) { return (int)lroundf(v * S); }
static int PX(float size) { return MAX(6, (int)lroundf(size * S)); }

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void clip(int x, int y, int w, int h) {
    cx0 = MAX(0, x), cy0 = MAX(0, y), cx1 = MIN(W, x + w), cy1 = MIN(H, y + h);
}
static void noclip(void) { cx0 = cy0 = 0, cx1 = W, cy1 = H; }

static uint32_t with_a(uint32_t c, int a) { return (c & 0xFFFFFF) | (uint32_t)CLAMP(a, 0, 255) << 24; }

// c over d, c's alpha times a/256 (c not premultiplied)
static inline uint32_t mixc(uint32_t d, uint32_t c, int a) {
    if (a >= 255) return c | 0xFF000000;
    if (a <= 0) return d;
    uint32_t rb = (((c & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8) & 0xFF00FF;
    uint32_t g = (((c & 0xFF00) * a + (d & 0xFF00) * (256 - a)) >> 8) & 0xFF00;
    return 0xFF000000 | rb | g;
}

// premultiplied s over d, s scaled by k/256 first
static inline uint32_t over(uint32_t d, uint32_t s, int k) {
    if (k < 256) {
        uint32_t rb = ((s & 0xFF00FF) * k >> 8) & 0xFF00FF;
        uint32_t ag = ((s >> 8 & 0xFF00FF) * k) & 0xFF00FF00;
        s = rb | ag;
    }
    uint32_t a = s >> 24;
    if (!a) return d;
    if (a == 255) return s;
    uint32_t ia = 256 - a;
    uint32_t rb = ((d & 0xFF00FF) * ia >> 8) & 0xFF00FF;
    uint32_t g = ((d & 0xFF00) * ia >> 8) & 0xFF00;
    return 0xFF000000 | ((s & 0xFFFFFF) + rb + g);
}

static void fill(int x, int y, int w, int h, uint32_t c) {
    int a = (int)(c >> 24) * g_alpha / 256;
    a += a >> 7; // 255 -> 256
    int x0 = MAX(x, cx0), y0 = MAX(y, cy0), x1 = MIN(x + w, cx1), y1 = MIN(y + h, cy1);
    for (int j = y0; j < y1; j++) {
        uint32_t *p = canvas + (size_t)j * W;
        if (a >= 256)
            for (int i = x0; i < x1; i++) p[i] = c | 0xFF000000;
        else
            for (int i = x0; i < x1; i++) p[i] = mixc(p[i], c, a);
    }
}

static void pixel(int x, int y, uint32_t c, int a) { // a 0-256
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
    a = a * (int)(c >> 24) / 255 * g_alpha / 256;
    uint32_t *p = canvas + (size_t)y * W + x;
    *p = mixc(*p, c, a);
}

// Inset of a rounded corner on row j of a box h tall (fractional, for smooth edges)
static float corner(int j, int h, int r) {
    if (r <= 0 || (j >= r && j < h - r)) return 0;
    float dy = j < r ? r - j - 0.5f : j - (h - r) + 0.5f;
    return r - sqrtf(MAX(0.f, (float)r * r - dy * dy));
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c) {
    r = MIN(r, MIN(w, h) / 2);
    for (int j = 0; j < h; j++) {
        float f = corner(j, h, r);
        int i = (int)f;
        int edge = (int)((1 - (f - i)) * 256);
        fill(x + i + 1, y + j, w - 2 * i - 2, 1, c);
        pixel(x + i, y + j, c, edge);
        pixel(x + w - 1 - i, y + j, c, edge);
    }
}

static void rrect_line(int x, int y, int w, int h, int r, int t, uint32_t c) {
    r = MIN(r, MIN(w, h) / 2);
    int ri = MAX(0, r - t);
    for (int j = 0; j < h; j++) {
        float f = corner(j, h, r);
        int i = (int)f;
        int edge = (int)((1 - (f - i)) * 256);
        pixel(x + i, y + j, c, edge);
        pixel(x + w - 1 - i, y + j, c, edge);
        if (j < t || j >= h - t) {
            fill(x + i + 1, y + j, w - 2 * i - 2, 1, c);
        } else {
            int k = (int)ceilf(corner(j - t, h - 2 * t, ri)) + t;
            fill(x + i + 1, y + j, k - i - 1, 1, c);
            fill(x + w - k, y + j, k - i - 1, 1, c);
        }
    }
}

static void disc(float cx, float cy, float r, uint32_t c) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = sqrtf((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy));
            float a = CLAMP(r - d + .5f, 0, 1);
            if (a > 0) pixel(x, y, c, (int)(a * 256));
        }
}

// A thick line with round ends (screen pixels)
static void line(float x0, float y0, float x1, float y1, float t, uint32_t c) {
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, hw = t / 2;
    for (int y = (int)(MIN(y0, y1) - hw - 1); y <= (int)(MAX(y0, y1) + hw + 1); y++)
        for (int x = (int)(MIN(x0, x1) - hw - 1); x <= (int)(MAX(x0, x1) + hw + 1); x++) {
            float px = x + .5f - x0, py = y + .5f - y0;
            float u = len2 > 0 ? CLAMP((px * dx + py * dy) / len2, 0, 1) : 0;
            float ex = px - u * dx, ey = py - u * dy;
            float a = CLAMP(hw - sqrtf(ex * ex + ey * ey) + .5f, 0, 1);
            if (a > 0) pixel(x, y, c, (int)(a * 256));
        }
}

// Darkens the whole canvas (under a dialog)
static void darken(void) {
    for (size_t i = 0, n = (size_t)W * H; i < n; i++) // x 3/8
        canvas[i] = 0xFF000000 | ((canvas[i] >> 1 & 0x7F7F7F) - (canvas[i] >> 3 & 0x1F1F1F));
}

// ---------------------------------------------------------------- pictures

typedef struct { int w, h; uint32_t *px; } Img; // premultiplied ARGB

static Img img_new(int w, int h) { return (Img){w, h, calloc((size_t)w * h, 4)}; }

static Img img_halve(Img s) {
    Img d = img_new(MAX(1, s.w / 2), MAX(1, s.h / 2));
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            const uint32_t *a = s.px + (size_t)(y * 2) * s.w + x * 2, *b = a + s.w;
            uint32_t p[4] = {a[0], a[1], b[0], b[1]}, o = 0;
            for (int sh = 0; sh < 32; sh += 8)
                o |= (((p[0] >> sh & 255) + (p[1] >> sh & 255) + (p[2] >> sh & 255) + (p[3] >> sh & 255) + 2) / 4) << sh;
            d.px[(size_t)y * d.w + x] = o;
        }
    return d;
}

// Smooth resize (halving first, so shrinking a big cover stays sharp but not noisy)
static Img img_resize(Img s, int w, int h) {
    Img cur = s;
    while (cur.w >= w * 2 && cur.h >= h * 2) {
        Img n = img_halve(cur);
        if (cur.px != s.px) free(cur.px);
        cur = n;
    }
    Img d = img_new(w, h);
    for (int y = 0; y < h; y++) {
        float fy = CLAMP((y + .5f) * cur.h / h - .5f, 0, cur.h - 1);
        int y0 = (int)fy, y1 = MIN(y0 + 1, cur.h - 1);
        int wy = (int)((fy - y0) * 256);
        for (int x = 0; x < w; x++) {
            float fx = CLAMP((x + .5f) * cur.w / w - .5f, 0, cur.w - 1);
            int x0 = (int)fx, x1 = MIN(x0 + 1, cur.w - 1);
            int wx = (int)((fx - x0) * 256);
            uint32_t a = cur.px[(size_t)y0 * cur.w + x0], b = cur.px[(size_t)y0 * cur.w + x1];
            uint32_t c = cur.px[(size_t)y1 * cur.w + x0], e = cur.px[(size_t)y1 * cur.w + x1], o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                int top = (int)(a >> sh & 255) * (256 - wx) + (int)(b >> sh & 255) * wx;
                int bot = (int)(c >> sh & 255) * (256 - wx) + (int)(e >> sh & 255) * wx;
                o |= (uint32_t)((top * (256 - wy) + bot * wy) >> 16) << sh;
            }
            d.px[(size_t)y * w + x] = o;
        }
    }
    if (cur.px != s.px) free(cur.px);
    return d;
}

enum { FIT, STRETCH };

// A picture file, shrunk (or grown) to fit bw x bh - or stretched to it
static Img img_load(const char *path, int bw, int bh, int mode) {
    int w, h, n;
    unsigned char *d = stbi_load(path, &w, &h, &n, 4);
    if (!d) return (Img){0};
    Img s = {w, h, (uint32_t *)d};
    for (size_t i = 0, k = (size_t)w * h; i < k; i++) { // RGBA bytes -> premultiplied ARGB
        unsigned char *p = d + i * 4;
        uint32_t a = p[3];
        s.px[i] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
    }
    int dw = bw, dh = bh;
    if (mode == FIT) {
        float k = MIN((float)bw / w, (float)bh / h);
        dw = MAX(1, (int)lroundf(w * k)), dh = MAX(1, (int)lroundf(h * k));
    }
    Img r = img_resize(s, dw, dh);
    free(d);
    return r;
}

// Draws a picture at x,y scaled to w x h (nearest pixel: cheap enough to do every frame),
// faded by alpha and darkened by shade (both 0-256)
static void blit(const Img *im, int x, int y, int w, int h, int alpha, int shade) {
    if (!im->px || w <= 0 || h <= 0) return;
    alpha = alpha * g_alpha / 256;
    int x0 = MAX(x, cx0), y0 = MAX(y, cy0), x1 = MIN(x + w, cx1), y1 = MIN(y + h, cy1);
    for (int j = y0; j < y1; j++) {
        const uint32_t *src = im->px + (size_t)((j - y) * im->h / h) * im->w;
        uint32_t *p = canvas + (size_t)j * W;
        for (int i = x0; i < x1; i++) {
            uint32_t s = src[(i - x) * im->w / w];
            if (shade < 256) s = (s & 0xFF000000) | ((((s & 0xFF00FF) * shade) >> 8) & 0xFF00FF) | ((((s & 0xFF00) * shade) >> 8) & 0xFF00);
            p[i] = over(p[i], s, alpha);
        }
    }
}

// The bottom of a picture, upside down and fading out: a reflection on a shiny floor
static void reflect(const Img *im, int x, int y, int w, int h, int rows, int alpha) {
    if (!im->px || w <= 0 || h <= 0) return;
    for (int r = 0; r < rows; r++) {
        int j = y + r;
        if (j < cy0 || j >= cy1) continue;
        int a = alpha * (rows - r) / rows * g_alpha / 256;
        const uint32_t *src = im->px + (size_t)((h - 1 - r) * im->h / h) * im->w;
        uint32_t *p = canvas + (size_t)j * W;
        for (int i = MAX(x, cx0); i < MIN(x + w, cx1); i++) p[i] = over(p[i], src[(i - x) * im->w / w], a);
    }
}

// Pictures are decoded and resized on a second thread, so the screen never waits for a
// cover: until one is ready its place stays empty, and the screen redraws when it is.
enum { P_EMPTY, P_QUEUED, P_LOADING, P_READY, P_FAILED };
typedef struct { char path[512]; int bw, bh, mode, state; unsigned used; Img img; } Pic;
#define NPIC 128
static Pic pics[NPIC];
static int jobs[NPIC], njobs;
static pthread_mutex_t pic_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pic_cv = PTHREAD_COND_INITIALIZER;
static int wake[2] = {-1, -1};
static int threaded;
static unsigned tick_no;

static void *loader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pic_mu);
    for (;;) {
        while (!njobs) pthread_cond_wait(&pic_cv, &pic_mu);
        Pic *p = &pics[jobs[0]];
        memmove(jobs, jobs + 1, --njobs * sizeof *jobs);
        p->state = P_LOADING;
        char path[512];
        int bw = p->bw, bh = p->bh, mode = p->mode;
        memcpy(path, p->path, sizeof path);
        pthread_mutex_unlock(&pic_mu);
        Img im = img_load(path, bw, bh, mode);
        pthread_mutex_lock(&pic_mu);
        p->img = im;
        p->state = im.px ? P_READY : P_FAILED;
        if (wake[1] >= 0 && write(wake[1], "", 1) < 0) { /* the screen redraws later anyway */ }
    }
    return NULL;
}

// The picture, or NULL while it loads (sync: load it now)
static const Img *pic(const char *path, int bw, int bh, int mode, int sync) {
    if (!path || !*path) return NULL;
    pthread_mutex_lock(&pic_mu);
    Pic *p = NULL, *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < NPIC; i++) {
        Pic *q = &pics[i];
        if (q->state == P_EMPTY) { if (!free_slot) free_slot = q; continue; }
        if (q->bw == bw && q->bh == bh && q->mode == mode && !strcmp(q->path, path)) { p = q; break; }
        if ((q->state == P_READY || q->state == P_FAILED) && q->used != tick_no && (!oldest || q->used < oldest->used)) oldest = q;
    }
    if (!p) {
        p = free_slot ? free_slot : oldest;
        if (!p) { pthread_mutex_unlock(&pic_mu); return NULL; }
        free(p->img.px);
        memset(p, 0, sizeof *p);
        snprintf(p->path, sizeof p->path, "%s", path);
        p->bw = bw, p->bh = bh, p->mode = mode;
        if (threaded && !sync) {
            p->state = P_QUEUED;
            jobs[njobs++] = (int)(p - pics);
            pthread_cond_signal(&pic_cv);
        } else {
            p->state = P_LOADING;
            pthread_mutex_unlock(&pic_mu);
            Img im = img_load(path, bw, bh, mode);
            pthread_mutex_lock(&pic_mu);
            p->img = im;
            p->state = im.px ? P_READY : P_FAILED;
        }
    }
    p->used = tick_no;
    const Img *r = p->state == P_READY ? &p->img : NULL;
    pthread_mutex_unlock(&pic_mu);
    return r;
}

// A new screen: covers asked for by the last one but not started yet are dropped
static void pic_drop_queue(void) {
    pthread_mutex_lock(&pic_mu);
    for (int i = 0; i < njobs; i++) pics[jobs[i]].state = P_EMPTY;
    njobs = 0;
    pthread_mutex_unlock(&pic_mu);
}

static void pic_flush(void) {
    pthread_mutex_lock(&pic_mu);
    njobs = 0;
    for (int i = 0; i < NPIC; i++)
        if (pics[i].state != P_LOADING) {
            free(pics[i].img.px);
            memset(&pics[i], 0, sizeof pics[i]);
        }
    pthread_mutex_unlock(&pic_mu);
}

// ---------------------------------------------------------------- text

static stbtt_fontinfo font;
static unsigned char *font_data;
static char font_path[512];
static float font_asc, font_desc; // per pixel of size

typedef struct { int size, w, h, xo, yo; uint32_t cp; float adv; unsigned char *bmp; } Glyph;
#define NGLYPH 4096
static Glyph glyphs[NGLYPH];

static void font_load(const char *path) {
    if (!strcmp(path, font_path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n || !stbtt_InitFont(&font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
        free(d);
        fclose(f);
        return;
    }
    fclose(f);
    free(font_data);
    font_data = d;
    snprintf(font_path, sizeof font_path, "%s", path);
    for (int i = 0; i < NGLYPH; i++) free(glyphs[i].bmp);
    memset(glyphs, 0, sizeof glyphs);
    int a, de, g;
    stbtt_GetFontVMetrics(&font, &a, &de, &g);
    float k = stbtt_ScaleForMappingEmToPixels(&font, 1);
    font_asc = a * k, font_desc = de * k;
}

static Glyph *glyph(int size, uint32_t cp) {
    unsigned h = (unsigned)(size * 2654435761u ^ cp * 40503u) & (NGLYPH - 1);
    for (int n = 0; n < NGLYPH; n++, h = (h + 1) & (NGLYPH - 1)) {
        Glyph *g = &glyphs[h];
        if (g->size == size && g->cp == cp) return g;
        if (g->size) continue;
        float k = stbtt_ScaleForMappingEmToPixels(&font, size);
        int gi = stbtt_FindGlyphIndex(&font, cp);
        if (!gi && cp > ' ') gi = stbtt_FindGlyphIndex(&font, '?');
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&font, gi, &adv, &lsb);
        g->size = size, g->cp = cp, g->adv = adv * k;
        g->bmp = stbtt_GetGlyphBitmap(&font, k, k, gi, &g->w, &g->h, &g->xo, &g->yo);
        return g;
    }
    for (int i = 0; i < NGLYPH; i++) free(glyphs[i].bmp); // full: start over
    memset(glyphs, 0, sizeof glyphs);
    return glyph(size, cp);
}

// One character from UTF-8 (a stray Latin-1 byte counts as itself)
static uint32_t utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    for (int i = 0; i < n; i++)
        if ((p[i] & 0xC0) != 0x80) n = 0;
    if (n) {
        c &= 0x3F >> n;
        for (int i = 0; i < n; i++) c = c << 6 | (*p++ & 63);
    }
    *s = (const char *)p;
    return c;
}

static float text_w(int size, const char *s) {
    if (!font_data) return 0;
    float w = 0;
    while (*s) w += glyph(size, utf8(&s))->adv;
    return w;
}

// The baseline that centres text of this size on y
static int baseline(int size, int cy) { return cy + (int)lroundf((font_asc + font_desc) * size / 2); }

// Draws text; returns where it ends
static float text(int size, float x, int base, uint32_t c, const char *s) {
    if (!font_data) return x;
    int ca = (int)(c >> 24) * g_alpha / 256;
    while (*s) {
        Glyph *g = glyph(size, utf8(&s));
        int gx = (int)lroundf(x) + g->xo, gy = base + g->yo;
        int x0 = MAX(gx, cx0), y0 = MAX(gy, cy0), x1 = MIN(gx + g->w, cx1), y1 = MIN(gy + g->h, cy1);
        for (int j = y0; j < y1; j++) {
            const unsigned char *b = g->bmp + (size_t)(j - gy) * g->w;
            uint32_t *p = canvas + (size_t)j * W;
            for (int i = x0; i < x1; i++) {
                int a = b[i - gx];
                if (a) p[i] = mixc(p[i], c, (a + (a >> 7)) * ca >> 8);
            }
        }
        x += g->adv;
    }
    return x;
}

static void text_bold(int size, float x, int base, uint32_t c, const char *s) {
    text(size, x, base, c, s);
    text(size, x + MAX(1, L(1)), base, c, s);
}

static void text_center(int size, float cx, int cy, uint32_t c, const char *s) {
    text(size, cx - text_w(size, s) / 2, baseline(size, cy), c, s);
}

// ---------------------------------------------------------------- frames

#define MAXITEMS 48
#define MAXCOVERS 96
#define TXT 512

typedef struct {
    char status[TXT], footer[TXT], statusbar[TXT], transition[8];
    int nitems, highlight, list_start, scrollbar, progress, total;
    char items[MAXITEMS][TXT], list_key[TXT];
    int art; // 0 none, 1 empty space, 2 picture
    char art_path[TXT];
    int osk, osk_x, osk_y, nkeys;
    char osk_buf[TXT], osk_hint[TXT], osk_keys[42][8];
    int cf, cf_sel, cf_total, ncovers;
    char cf_key[TXT], cf_label[TXT];
    struct { int idx; char path[TXT]; } covers[MAXCOVERS];
    int dialog, ndlines, ndbtns, dsel;
    char dtitle[TXT], dlines[8][TXT], dbtns[4][64];
    int ntiles, tile_sel, tile_press;
    char tile_dir[TXT];
    struct { char icon[64], title[128], sub[128]; } tiles[6];
    int boot, boot_progress, nboot_info;
    char boot_ver[64], boot_step[TXT], boot_tip[TXT], boot_credit[TXT], boot_thanks[TXT], boot_info[2][TXT];
    int busy;
    char busy_text[TXT];
} Frame;

static Frame incoming, cur, last; // last: the screen before cur (for list slides)
static int has_cur, dialog_on;
static Frame dlg;

// Settings that last from frame to frame
static uint32_t C_BG = 0xFF051105, C_TEXT = 0xFF33FF33, C_HL = 0xFF33FF33, C_HLT = 0xFF051105, C_DIM = 0xFF33FF33;
static int base_size = 22, motion = 2, perf;
static char bg_path[TXT], title[128] = "ROM MANAGER", ui_dir[TXT];

static void copy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s); }
#define COPY(d, s) copy(d, sizeof(d), s)

static void frame_reset(Frame *f) {
    memset(f, 0, sizeof *f);
    f->highlight = -1, f->scrollbar = -1, f->progress = -1, f->total = -1;
}

static uint32_t color(const char *v) { return (uint32_t)strtoul(v, NULL, 16) | 0xFF000000; }

// One line of a frame
static void parse(Frame *f, char *k, char *v) {
#define IS(s) (!strcmp(k, s))
    if (IS("THEME_BG")) C_BG = color(v);
    else if (IS("THEME_TEXT")) C_TEXT = color(v);
    else if (IS("THEME_HL")) C_HL = color(v);
    else if (IS("THEME_HL_TEXT")) C_HLT = color(v);
    else if (IS("THEME_DIM")) C_DIM = color(v);
    else if (IS("FONT")) {
        char *c = strrchr(v, ':');
        if (c) { *c = 0; base_size = CLAMP(atoi(c + 1), 8, 64); }
        font_load(v);
    } else if (IS("BACKGROUND")) {
        COPY(bg_path, v);
        COPY(ui_dir, v);
        char *s = strrchr(ui_dir, '/');
        if (s) *s = 0;
    } else if (IS("TITLE")) COPY(title, v);
    else if (IS("MOTION")) motion = atoi(v);
    else if (IS("PERF")) perf = atoi(v);
    else if (IS("STATUSBAR")) COPY(f->statusbar, v);
    else if (IS("STATUS")) COPY(f->status, v);
    else if (IS("FOOTER")) COPY(f->footer, v);
    else if (IS("TRANSITION")) COPY(f->transition, v);
    else if (IS("ITEM")) { if (f->nitems < MAXITEMS) COPY(f->items[f->nitems++], v); }
    else if (IS("HIGHLIGHT")) f->highlight = atoi(v);
    else if (IS("LISTPOS")) {
        f->list_start = atoi(v);
        char *c = strchr(v, ':');
        COPY(f->list_key, c ? c + 1 : "");
        char *d = strrchr(f->list_key, '-');
        if (d) f->total = atoi(d + 1);
    } else if (IS("SCROLLBAR")) f->scrollbar = atoi(v);
    else if (IS("ART")) {
        if (!strcmp(v, "NULL")) f->art = 0;
        else if (!strcmp(v, "BLANK") || !*v) f->art = 1;
        else f->art = 2, COPY(f->art_path, v);
    } else if (IS("PROGRESS")) f->progress = atoi(v);
    else if (IS("BUSY")) f->busy = 1, COPY(f->busy_text, v);
    else if (IS("OSK")) {
        f->osk = 1;
        f->osk_x = atoi(v);
        char *a = strchr(v, ':'), *b = a ? strchr(a + 1, ':') : NULL;
        f->osk_y = a ? atoi(a + 1) : 0;
        COPY(f->osk_buf, b ? b + 1 : "");
    } else if (IS("OSK_HINT")) COPY(f->osk_hint, v);
    else if (IS("OSK_KEYS")) {
        f->nkeys = 0;
        for (char *t = strtok(v, " "); t && f->nkeys < 42; t = strtok(NULL, " ")) COPY(f->osk_keys[f->nkeys++], t);
    } else if (IS("COVERFLOW")) {
        f->cf = 1;
        f->cf_sel = atoi(v);
        char *a = strchr(v, ':'), *b = a ? strchr(a + 1, ':') : NULL;
        f->cf_total = a ? atoi(a + 1) : 1;
        COPY(f->cf_key, b ? b + 1 : "");
    } else if (IS("COVER")) {
        char *a = strchr(v, ':');
        if (a && f->ncovers < MAXCOVERS) {
            f->covers[f->ncovers].idx = atoi(v);
            COPY(f->covers[f->ncovers++].path, a + 1);
        }
    } else if (IS("CFLABEL")) COPY(f->cf_label, v);
    else if (IS("DIALOG")) f->dialog = 1, COPY(f->dtitle, v);
    else if (IS("DIALOG_LINE")) { if (f->ndlines < 8) COPY(f->dlines[f->ndlines++], v); }
    else if (IS("DIALOG_BTN")) { if (f->ndbtns < 4) COPY(f->dbtns[f->ndbtns++], v); }
    else if (IS("DIALOG_SEL")) f->dsel = atoi(v);
    else if (IS("TILE_ICONS")) COPY(f->tile_dir, v);
    else if (IS("TILE")) {
        if (f->ntiles < 6) {
            char *a = strchr(v, '|'), *b = a ? strchr(a + 1, '|') : NULL;
            if (a) *a = 0;
            if (b) *b = 0;
            COPY(f->tiles[f->ntiles].icon, v);
            COPY(f->tiles[f->ntiles].title, a ? a + 1 : "");
            COPY(f->tiles[f->ntiles++].sub, b ? b + 1 : "");
        }
    } else if (IS("TILE_SEL")) f->tile_sel = atoi(v);
    else if (IS("TILE_PRESS")) f->tile_press = atoi(v);
    else if (IS("BOOT")) f->boot = 1, COPY(f->boot_ver, v);
    else if (IS("BOOT_STEP")) COPY(f->boot_step, v);
    else if (IS("BOOT_PROGRESS")) f->boot_progress = atoi(v);
    else if (IS("BOOT_INFO")) { if (f->nboot_info < 2) COPY(f->boot_info[f->nboot_info++], v); }
    else if (IS("BOOT_TIP")) COPY(f->boot_tip, v);
    else if (IS("BOOT_CREDIT")) COPY(f->boot_credit, v);
    else if (IS("BOOT_THANKS")) COPY(f->boot_thanks, v);
#undef IS
}

// ---------------------------------------------------------------- animation state

static long t_now, t_frame;          // now; when the current screen arrived
static int need_fast, need_slow;     // set while drawing: something moves
static float list_off, hl_pos;       // list scroll and highlight bar (rows, animated)
static float cf_pos;                 // cover flow position (animated)
static float boot_shown;             // boot loading bar (animated)
static float tile_glow[6];
static long trans_t0, dialog_t0, toast_t0, letter_t0, marquee_t0, last_draw;
static char trans_kind[8];
static int trans_full;               // the boot screen slides as a whole
static char toast_kind[8], toast_text[TXT], letter[8];
static char marquee_text[TXT];
static int dl_pct = -1, dl_left;     // /tmp/rm_dlprog
static long dl_read_t;
static int freeze;                   // previews: everything at rest

static float ease(float t) { t = CLAMP(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }

// 0..1 progress of an animation that started at t0 and lasts ms (1 when motion is off)
static float anim(long t0, int ms) {
    if (freeze || motion == 0 || ms <= 0) return 1;
    float p = (float)(t_now - t0) / ms;
    if (p < 1) need_fast = 1;
    return CLAMP(p, 0, 1);
}

static void read_dlprog(void) {
    if (t_now - dl_read_t < 400 && dl_read_t) return;
    dl_read_t = t_now;
    FILE *f = fopen("/tmp/rm_dlprog", "r");
    dl_pct = -1, dl_left = 0;
    if (f) {
        if (fscanf(f, "%d|%d", &dl_pct, &dl_left) < 1) dl_pct = -1;
        fclose(f);
    }
}

// ---------------------------------------------------------------- drawing: pieces

static uint32_t C_ERR = 0xFFFF6040;

static void icon_check(float x, float cy, float s, uint32_t c) { // s: height, screen px
    line(x, cy, x + s * .38f, cy + s * .42f, MAX(2.f, s * .18f), c);
    line(x + s * .38f, cy + s * .42f, x + s, cy - s * .48f, MAX(2.f, s * .18f), c);
}

static void icon_wifi(float cx, float by, float s, uint32_t c) { // s: height
    for (int y = (int)(by - s - 1); y <= (int)by; y++)
        for (int x = (int)(cx - s - 1); x <= (int)(cx + s + 1); x++) {
            float dx = x + .5f - cx, dy = by - (y + .5f), r = sqrtf(dx * dx + dy * dy);
            if (fabsf(dx) > dy + 1) continue;
            float u = r / s * 4; // dot, then three arcs
            int on = u < .55f || (u > 1.25f && u < 1.85f) || (u > 2.35f && u < 2.95f) || (u > 3.45f && u < 4.0f);
            if (on) pixel(x, y, c, 256);
        }
}

static void icon_battery(int x, int cy, int w, int h, int pct, uint32_t c) {
    int t = MAX(1, L(1.5f));
    rrect_line(x, cy - h / 2, w, h, L(2), t, c);
    fill(x + w, cy - h / 4, MAX(2, L(2)), h / 2, c);
    int inner = w - 4 * t;
    fill(x + 2 * t, cy - h / 2 + 2 * t, inner * CLAMP(pct, 0, 100) / 100, h - 4 * t, pct < 20 ? C_ERR : c);
}

static void icon_download(float cx, float cy, float s, uint32_t c) {
    float t = MAX(2.f, s * .16f);
    line(cx, cy - s * .5f, cx, cy + s * .15f, t, c);
    line(cx - s * .32f, cy - s * .12f, cx, cy + s * .2f, t, c);
    line(cx + s * .32f, cy - s * .12f, cx, cy + s * .2f, t, c);
    line(cx - s * .45f, cy + s * .48f, cx + s * .45f, cy + s * .48f, t, c);
}

static void spinner(float cx, float cy, float r, float dot, uint32_t c) {
    int phase = (int)(t_now / 80) % 12;
    for (int i = 0; i < 12; i++) {
        float a = i * (float)M_PI / 6;
        int k = (i - phase + 12) % 12;
        disc(cx + sinf(a) * r, cy - cosf(a) * r, dot, with_a(c, 40 + 215 * (12 - k) / 12));
    }
    need_slow = 1;
}

// "{DL}" as text: "45%" and how many more are waiting
static void dl_text(char *out, size_t n) {
    read_dlprog();
    if (dl_pct < 0) snprintf(out, n, "...");
    else if (dl_left > 1) snprintf(out, n, "%d%% +%d", dl_pct, dl_left - 1);
    else snprintf(out, n, "%d%%", dl_pct);
}

// Top right: download, Wi-Fi, battery, free space
static void draw_statusbar(const char *s) {
    char buf[TXT], parts[8][64];
    int np = 0;
    COPY(buf, s);
    for (char *t = strtok(buf, " "); t && np < 8; t = strtok(NULL, " ")) COPY(parts[np++], t);
    int size = PX(14), gap = L(9), cy = Y(30);
    float wid[8], total = 0;
    char dl[32] = "";
    for (int i = 0; i < np; i++) {
        if (!strcmp(parts[i], "{DL}")) dl_text(dl, sizeof dl), wid[i] = L(16) + text_w(size, dl);
        else if (!strcmp(parts[i], "{WIFI}") || !strcmp(parts[i], "{NOWIFI}")) wid[i] = L(16);
        else if (!strncmp(parts[i], "{BATT:", 6)) wid[i] = L(25);
        else wid[i] = text_w(size, parts[i]);
        total += wid[i] + (i ? gap : 0);
    }
    float x = X(626) - total;
    for (int i = 0; i < np; i++) {
        const char *p = parts[i];
        if (!strcmp(p, "{DL}")) {
            icon_download(x + L(6), cy, L(12), C_TEXT);
            text(size, x + L(15), baseline(size, cy), C_TEXT, dl);
        } else if (!strcmp(p, "{WIFI}")) icon_wifi(x + L(8), cy + L(6), L(13), C_TEXT);
        else if (!strcmp(p, "{NOWIFI}")) icon_wifi(x + L(8), cy + L(6), L(13), with_a(C_DIM, 0x55));
        else if (!strncmp(p, "{BATT:", 6)) icon_battery((int)x, cy, L(20), L(11), atoi(p + 6), C_TEXT);
        else text(size, x, baseline(size, cy), C_TEXT, p);
        x += wid[i] + gap;
    }
}

// A button: a round one for A B X Y L R, a pill for Start / Sel / L2...
static float button(float x, int cy, const char *key, int size) {
    int d = L(18), r = d / 2;
    int ks = MAX(6, size * 13 / 19);
    if (!strcmp(key, "<>") || !strcmp(key, "^v")) { // the d-pad: two triangles
        int w = L(30);
        rrect((int)x, cy - r, w, d, r, C_HL);
        int a = L(5), m = (int)x + w / 2, gap = L(2);
        if (key[0] == '<')
            for (int j = -a; j <= a; j++) { // < >
                int len = a - abs(j);
                fill(m - gap - len, cy + j, len, 1, C_HLT);
                fill(m + gap, cy + j, len, 1, C_HLT);
            }
        else
            for (int j = 0; j <= a; j++) { // up, down
                fill(m - gap - a - j, cy - a / 2 + j, 2 * j + 1, 1, C_HLT);
                fill(m + gap + j, cy - a / 2 + j, 2 * (a - j) + 1, 1, C_HLT);
            }
        return x + w;
    }
    float tw = text_w(ks, key);
    if (strlen(key) == 1) {
        disc(x + r, cy, r, C_HL);
        text(ks, x + r - tw / 2, baseline(ks, cy), C_HLT, key);
        return x + d;
    }
    int w = (int)tw + L(14);
    rrect((int)x, cy - r, w, d, r, C_HL);
    text(ks, x + L(7), baseline(ks, cy), C_HLT, key);
    return x + w;
}

// "A/ Play  X/ Scrape  B/ Back": buttons and what they do
static void draw_footer(const char *s) {
    char tok[16][128];
    int n = 0;
    const char *p = s;
    while (*p && n < 16) {
        const char *e = strstr(p, "  ");
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) snprintf(tok[n++], sizeof tok[0], "%.*s", (int)MIN(len, 127), p);
        if (!e) break;
        p = e;
        while (*p == ' ') p++;
    }
    int size = PX(19), cy = Y(450);
    for (int pass = 0; pass < 2; pass++) { // second pass: smaller, if it didn't fit
        float x = X(22), gap = L(16);
        int draw = pass == 1;
        for (int i = 0; i < n; i++) {
            char key[32] = "", *label = tok[i], *sl = strstr(tok[i], "/ ");
            if (sl && sl - tok[i] < 8) {
                snprintf(key, sizeof key, "%.*s", (int)(sl - tok[i]), tok[i]);
                label = sl + 2;
            }
            if (*key) {
                if (draw) x = button(x, cy, key, size);
                else x += strlen(key) == 1 ? L(18) : !strcmp(key, "<>") || !strcmp(key, "^v") ? L(30) : text_w(MAX(6, size * 13 / 19), key) + L(14);
                x += L(5);
            }
            if (draw) text(size, x, baseline(size, cy), C_TEXT, label);
            x += text_w(size, label) + gap;
        }
        if (!draw) {
            float avail = X(626) - X(22), used = x - X(22);
            if (used > avail) size = MAX(PX(13), (int)(size * avail / used));
        }
    }
}

// What stays the same from screen to screen: the background, the boxes, the title.
// Kept drawn, so a frame starts with one copy.
static uint32_t *chrome[3];
static char chrome_key[3][TXT + 160];

static void draw_chrome(int kind) { // 0 background only, 1 + header and status, 2 + panel
    char key[TXT + 160];
    snprintf(key, sizeof key, "%s|%08x%08x%08x|%s|%d", bg_path, C_BG, C_TEXT, C_HL, title, font_data != NULL);
    if (chrome[kind] && !strcmp(key, chrome_key[kind])) {
        memcpy(canvas, chrome[kind], (size_t)W * H * 4);
        return;
    }
    noclip();
    const Img *bg = pic(bg_path, W, H, STRETCH, 1);
    // (opaque: on some screens a see-through pixel shows what's under the framebuffer)
    for (size_t i = 0; i < (size_t)W * H; i++) canvas[i] = bg ? over(C_BG, bg->px[i], 256) : C_BG;
    if (kind >= 1) {
        rrect(X(10), Y(10), L(208), L(39), L(8), with_a(C_BG, 0xE0));
        rrect_line(X(10), Y(10), L(208), L(39), L(8), MAX(1, L(1.5f)), with_a(C_TEXT, 0x50));
        int ts = PX(30);
        float tw = text_w(ts, title);
        text_bold(ts, X(114) - tw / 2, baseline(ts, Y(30)), C_TEXT, title);
        fill(X(10), Y(55), L(620), MAX(2, L(2)), C_HL);
        fill(X(10), Y(404), L(620), L(30), with_a(C_BG, 0xF2));
    }
    if (kind == 2) fill(X(10), Y(72), L(620), L(330), with_a(C_BG, 0xE6));
    if (!chrome[kind]) chrome[kind] = malloc((size_t)W * H * 4);
    memcpy(chrome[kind], canvas, (size_t)W * H * 4);
    COPY(chrome_key[kind], key);
}

// ---------------------------------------------------------------- drawing: screens

static const char *item_text(const Frame *f, int abs) {
    if (abs >= f->list_start && abs < f->list_start + f->nitems) return f->items[abs - f->list_start];
    return NULL;
}

// One list row: check mark, text (scrolling if it's the chosen one and too long), volume bar
static void draw_row(const char *s, int x0, int x1, int y, int h, int size, uint32_t c, int scroll) {
    char t[TXT];
    COPY(t, s);
    int check = !strncmp(t, "{CHECK}", 7), bar = -1;
    char *b = strstr(t, "{BAR:");
    if (b) { bar = atoi(b + 5); *b = 0; }
    float tx = X(30);
    int cy = y + h / 2;
    if (check) {
        icon_check(tx + L(1), cy, L(12), c);
        tx += L(20);
    }
    const char *body = t + (check ? 7 : 0);
    float right = bar >= 0 ? x1 - L(150) : x1 - L(6);
    float tw = text_w(size, body), off = 0;
    if (scroll && tw > right - tx) {
        // wait, slide to the end, wait, start over
        float over_px = tw - (right - tx), speed = L(45) / 1000.f;
        long run = (long)(over_px / speed), cycle = 1100 + run + 1400;
        if (strcmp(marquee_text, body)) { COPY(marquee_text, body); marquee_t0 = t_now; }
        long t = freeze ? 0 : (t_now - marquee_t0) % cycle;
        off = t < 1100 ? 0 : t < 1100 + run ? (t - 1100) * speed : over_px;
        need_slow = 1;
    }
    int sx0 = cx0, sx1 = cx1;
    cx0 = MAX(cx0, (int)tx - L(2)), cx1 = MIN(cx1, (int)right);
    text(size, tx - off, baseline(size, cy), c, body);
    cx0 = sx0, cx1 = sx1;
    if (bar >= 0) {
        int bw = L(7), g = L(3), bx = x1 - L(12) - 10 * bw - 9 * g;
        for (int i = 0; i < 10; i++) {
            int bh = L(6 + i * 1.3f);
            fill(bx + i * (bw + g), cy + L(9) - bh, bw, bh, i < bar ? c : with_a(c, 0x40));
        }
    }
    (void)x0;
}

static void draw_list(const Frame *f) {
    int top = 74, bottom = 400;
    if (f->progress >= 0) {
        int x = X(30), y = Y(82), w = L(580), h = L(16);
        rrect_line(x, y, w, h, h / 2, MAX(1, L(1.5f)), C_HL);
        int in = MAX(2, L(3));
        rrect(x + in, y + in, (w - 2 * in) * CLAMP(f->progress, 0, 100) / 100, h - 2 * in, (h - 2 * in) / 2, C_HL);
        top = 108;
    }
    if (!f->nitems) return;
    float rh = MIN(30.f, (float)(bottom - top) / f->nitems);
    int size = MIN(PX(base_size), (int)(L(rh) * .78f));
    int x0 = X(14), x1 = f->art ? X(346) : X(626);
    int ly = Y(top), lh = Y(bottom) - ly;
    clip(x0, ly, x1 - x0, lh + L(2));
    int sel = f->highlight >= 0 ? f->list_start + f->highlight : -1;
    int first = (int)floorf(list_off), lastrow = (int)ceilf(list_off) + MAX(f->nitems, last.nitems) + 1;
    // rows (from this screen, or the last one while sliding)
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (sel < 0) break;
            int hy = ly + L((hl_pos - list_off) * rh);
            fill(x0, hy + 1, x1 - x0, L(rh) - 2, C_HL);
            int sy0 = cy0, sy1 = cy1;
            cy0 = MAX(cy0, hy + 1), cy1 = MIN(cy1, hy + L(rh) - 1);
            for (int a = first; a <= lastrow; a++) {
                const char *s = item_text(f, a);
                if (!s && list_off != f->list_start) s = item_text(&last, a);
                if (s) draw_row(s, x0, x1, ly + L((a - list_off) * rh), L(rh), size, C_HLT, a == sel);
            }
            cy0 = sy0, cy1 = sy1;
            break;
        }
        for (int a = first; a <= lastrow; a++) {
            const char *s = item_text(f, a);
            if (!s && list_off != f->list_start) s = item_text(&last, a);
            if (!s) continue;
            int y = ly + L((a - list_off) * rh);
            draw_row(s, x0, x1, y, L(rh), size, C_TEXT, 0);
            fill(x0 + L(2), y + L(rh) - 1, x1 - x0 - L(4), MAX(1, L(1)), with_a(C_DIM, 0x38));
        }
    }
    noclip();
    // where in the list: a short bar on the left edge
    if (f->scrollbar >= 0 && f->total > f->nitems) {
        float ty = 74 + (326 - 40) * CLAMP(f->scrollbar, 0, 100) / 100.f;
        rrect(X(3), Y(ty), L(4), L(40), L(2), C_HL);
    }
}

static void draw_art(const Frame *f) {
    if (f->art != 2) return;
    int bw = L(264), bh = L(312);
    const Img *im = pic(f->art_path, bw, bh, FIT, 0);
    if (!im) return;
    int x = X(490) - im->w / 2, y = Y(240) - im->h / 2;
    blit(im, x, y, im->w, im->h, 256, 256);
}

static void draw_busy(const Frame *f) {
    spinner(X(320), Y(210), L(24), L(3.6f), C_HL);
    int size = PX(base_size);
    text_center(size, X(320), Y(272), C_TEXT, f->busy_text);
}

// Console pictures (and "TEXT:name", a console with none) sit in a square frame
static int is_icon(const char *p) { return !strncmp(p, "ICON:", 5) || !strncmp(p, "TEXT:", 5) || strstr(p, "/ui/") != NULL; }

static void cover_path(const char *p, char *out, size_t n) {
    if (!strncmp(p, "ICON:", 5)) snprintf(out, n, "%s/menu/%s.png", ui_dir, p + 5);
    else snprintf(out, n, "%s", p);
}

static void draw_coverflow(const Frame *f) {
    clip(X(10), Y(72), L(620), L(330));
    // far ones first, the chosen one on top
    int order[MAXCOVERS], n = 0;
    for (int i = 0; i < f->ncovers; i++)
        if (fabsf(f->covers[i].idx - cf_pos) < 3.6f) order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && fabsf(f->covers[order[j]].idx - cf_pos) > fabsf(f->covers[order[j - 1]].idx - cf_pos); j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    int moving = fabsf(cf_pos - f->cf_sel) > .001f;
    float bob = 0;
    if (!moving && motion && !freeze) {
        bob = sinf((t_now - t_frame) * 2 * (float)M_PI / 2600) * 3;
        need_slow = 1;
    }
    for (int k = 0; k < n; k++) {
        const char *path = f->covers[order[k]].path;
        float d = f->covers[order[k]].idx - cf_pos, ad = fabsf(d);
        int icon = is_icon(path);
        float bw = icon ? 200 : 188, bh = icon ? 216 : 264;
        float sc = 1 - .32f * MIN(ad, 1) - .07f * CLAMP(ad - 1, 0, 2);
        float off = (d < 0 ? -1 : 1) * (ad <= 1 ? ad * 168 : 168 + (ad - 1) * 118);
        float cx = 320 + off, cy = (icon ? 222 : 211) + (ad < .5f ? bob * (1 - ad * 2) : 0);
        char full[TXT + 64];
        cover_path(path, full, sizeof full);
        int named = !strncmp(path, "TEXT:", 5);
        const Img *im = strcmp(path, "BLANK") && !named ? pic(full, L(bw), L(bh), FIT, 0) : NULL;
        int w = im ? (int)(im->w * sc) : L(bw * sc * (icon ? .8f : .72f)), h = im ? (int)(im->h * sc) : L(bh * sc * (icon ? .8f : 1));
        int x = X(cx) - w / 2, y = Y(cy) - h / 2;
        int shade = 256 - (int)(MIN(ad, 1.5f) * 70);
        if (im) {
            if (!icon) reflect(im, x, y + h + L(2), w, h, MIN(h * 3 / 10, L(60)), 80 * shade / 256);
            blit(im, x, y, w, h, 256, shade);
        } else {
            fill(x, y, w, h, with_a(mixc(C_BG, C_HL, 24), 0xF0));
            rrect_line(x, y, w, h, L(4), MAX(1, L(1.5f)), with_a(C_DIM, 0x60));
            if (named) { // two sizes only: every size drawn is kept in the glyph cache
                int ts = ad < .5f ? PX(34) : PX(26);
                float tw = text_w(ts, path + 5);
                if (tw > w - L(16)) ts = MAX(PX(12), (int)(ts * (w - L(16)) / tw));
                text_center(ts, x + w / 2.f, y + h / 2, with_a(C_TEXT, shade - 1), path + 5);
            }
        }
        if (ad < .5f) { // a frame around the chosen one
            int m = icon ? L(10) : L(3), t = MAX(2, L(2));
            rrect_line(x - m, y - m, w + 2 * m, h + 2 * m, L(2), t, with_a(C_HL, (int)(255 * (1 - ad * 2))));
        }
    }
    noclip();
    fill(X(50), Y(362), L(540), L(32), with_a(C_BG, 0xD8));
    int size = PX(21);
    float tw = text_w(size, f->cf_label);
    if (tw > L(520)) size = MAX(PX(14), (int)(size * L(520) / tw));
    clip(X(56), Y(362), L(528), L(32));
    text_center(size, X(320), Y(378), C_TEXT, f->cf_label);
    noclip();
}

static void draw_osk(const Frame *f) {
    int bx = X(20), by = Y(80), bw = L(600), bh = L(38), size = PX(base_size);
    rrect(bx, by, bw, bh, L(6), with_a(C_BG, 0xF0));
    rrect_line(bx, by, bw, bh, L(6), MAX(1, L(2)), C_HL);
    // the typed text (its end, if long), the rest of a suggestion greyed out, a cursor
    const char *s = f->osk_buf;
    float avail = bw - L(40);
    while (*s && text_w(size, s) > avail) s++;
    clip(bx + L(8), by, bw - L(16), bh);
    float x = text(size, bx + L(12), baseline(size, by + bh / 2), C_TEXT, s);
    size_t n = strlen(f->osk_buf);
    if (*f->osk_hint && !strncasecmp(f->osk_hint, f->osk_buf, n)) text(size, x, baseline(size, by + bh / 2), with_a(C_DIM, 0x70), f->osk_hint + n);
    if (freeze || (t_now / 530) % 2 == 0) fill((int)x + L(1), by + L(8), L(10), bh - L(16), C_HL);
    need_slow = 1;
    noclip();
    float kw = (600 - 6 * 6) / 7.f, kh = (264 - 5 * 6) / 6.f;
    int ks = PX(18);
    for (int i = 0; i < f->nkeys; i++) {
        int c = i % 7, r = i / 7;
        int x = X(20 + c * (kw + 6)), y = Y(132 + r * (kh + 6)), w = L(kw), h = L(kh);
        int on = c == f->osk_x && r == f->osk_y;
        rrect(x, y, w, h, L(5), on ? C_HL : with_a(C_HL, 0x1A));
        if (!on) rrect_line(x, y, w, h, L(5), MAX(1, L(1)), with_a(C_DIM, 0x50));
        const char *k = f->osk_keys[i];
        const char *label = !strcmp(k, "SPC") ? "space" : !strcmp(k, "CAPS") ? "caps" : k;
        text_center(ks, x + w / 2.f, y + h / 2, on ? C_HLT : C_TEXT, label);
    }
}

static void draw_tiles(const Frame *f) {
    for (int i = 0; i < f->ntiles && i < 6; i++) {
        float target = i == f->tile_sel;
        if (freeze || !motion) tile_glow[i] = target;
        else {
            float step = (t_now - last_draw) / 110.f;
            tile_glow[i] += CLAMP(target - tile_glow[i], -step, step);
            if (fabsf(tile_glow[i] - target) > .001f) need_fast = 1;
        }
        float g = CLAMP(tile_glow[i], 0, 1);
        int col = i % 3, row = i / 3;
        float tx = 24 + col * 204, ty = 78 + row * 170, tw = 186, th = 150;
        float grow = g * 3 - (i == f->tile_sel && f->tile_press ? 6 : 0);
        int x = X(tx - grow), y = Y(ty - grow), w = L(tw + 2 * grow), h = L(th + 2 * grow), r = L(9);
        if (g > 0) {
            rrect(x - L(4), y - L(4), w + L(8), h + L(8), r + L(4), with_a(C_HL, (int)(40 * g)));
            rrect(x - L(2), y - L(2), w + L(4), h + L(4), r + L(2), with_a(C_HL, (int)(60 * g)));
        }
        rrect(x, y, w, h, r, with_a(mixc(C_BG, C_HL, (int)(30 * g)), 0xE0));
        if (g > 0) rrect_line(x, y, w, h, r, MAX(2, L(2)), with_a(C_HL, (int)(255 * g)));
        char path[TXT + 80];
        snprintf(path, sizeof path, "%s/%s.png", f->tile_dir, f->tiles[i].icon);
        const Img *im = pic(path, L(74), L(74), FIT, 1);
        float mid = x + w / 2.f;
        if (im) blit(im, (int)(mid - im->w / 2.f), y + L(16), im->w, im->h, 256, 256);
        text_center(PX(20), mid, y + L(112), C_TEXT, f->tiles[i].title);
        char sub[128];
        if (!strcmp(f->tiles[i].sub, "{DL}")) {
            char d[32];
            dl_text(d, sizeof d);
            snprintf(sub, sizeof sub, "downloading %s", d);
        } else COPY(sub, f->tiles[i].sub);
        text_center(PX(14), mid, y + L(134), with_a(C_DIM, 0xC0), sub);
    }
}

static void draw_boot(const Frame *f) {
    int ts = PX(50);
    float tw = text_w(ts, title);
    text_bold(ts, X(320) - tw / 2, baseline(ts, Y(166)), C_TEXT, title);
    fill(X(320) - (int)(tw / 2), Y(196), (int)tw, MAX(2, L(2)), C_HL);
    text_center(PX(14), X(320), Y(217), with_a(C_DIM, 0xC0), f->boot_ver);
    float target = CLAMP(f->boot_progress, 0, 100);
    if (freeze || !motion || boot_shown > target) boot_shown = target;
    else {
        boot_shown += (target - boot_shown) * MIN(1.f, (t_now - last_draw) / 120.f);
        if (target - boot_shown > .3f) need_fast = 1;
        else boot_shown = target;
    }
    int x = X(150), y = Y(267), w = L(340), h = L(14), in = MAX(2, L(3));
    rrect_line(x, y, w, h, h / 2, MAX(1, L(1.5f)), C_HL);
    rrect(x + in, y + in, (int)((w - 2 * in) * boot_shown / 100), h - 2 * in, (h - 2 * in) / 2, C_HL);
    int ss = PX(16);
    float sw = text_w(ss, f->boot_step);
    float sx = X(320) - (sw + L(22)) / 2;
    if (f->boot_progress < 100) spinner(sx + L(7), Y(299), L(6), L(1.4f), C_HL);
    text(ss, sx + L(22), baseline(ss, Y(299)), C_TEXT, f->boot_step);
    for (int i = 0; i < f->nboot_info; i++) text_center(PX(base_size), X(320), Y(346 + i * 28), C_TEXT, f->boot_info[i]);
    text_center(PX(15), X(320), Y(423), with_a(C_DIM, 0xA8), f->boot_tip);
    text_center(PX(15), X(320), Y(447), with_a(C_DIM, 0xC8), f->boot_credit);
    text_center(PX(11), X(320), Y(466), with_a(C_DIM, 0x90), f->boot_thanks);
}

static void draw_dialog(const Frame *f) {
    float p = ease(anim(dialog_t0, 140));
    int size = PX(base_size);
    float maxw = text_w(size, f->dtitle);
    for (int i = 0; i < f->ndlines; i++) maxw = MAX(maxw, text_w(size, f->dlines[i]));
    float w = CLAMP(maxw / S + 64, 420, 600);
    float h = 64 + f->ndlines * 28 + 66;
    float sc = .94f + .06f * p;
    float x = 320 - w * sc / 2, y = 240 - h * sc / 2;
    g_alpha = (int)(256 * p);
    rrect(X(x), Y(y), L(w * sc), L(h * sc), L(12), with_a(C_BG, 0xF8));
    rrect_line(X(x), Y(y), L(w * sc), L(h * sc), L(12), MAX(2, L(2)), C_HL);
    text_center(size, X(320), Y(y + 30 * sc), C_TEXT, f->dtitle);
    fill(X(x + 16), Y(y + 48 * sc), L((w - 32) * sc), MAX(1, L(1)), with_a(C_DIM, 0x60));
    for (int i = 0; i < f->ndlines; i++) text_center(size, X(320), Y(y + (76 + i * 28) * sc), C_TEXT, f->dlines[i]);
    int nb = MAX(1, f->ndbtns);
    float gap = 22, bw = (w - 32 - (nb - 1) * gap) / nb, by = y + (64 + f->ndlines * 28 + 16) * sc;
    for (int i = 0; i < f->ndbtns; i++) {
        int bx = X(x + (16 + i * (bw + gap)) * sc), on = i == f->dsel;
        int ww = L(bw * sc), hh = L(34 * sc), yy = Y(by);
        if (on) rrect(bx, yy, ww, hh, L(7), C_HL);
        else rrect_line(bx, yy, ww, hh, L(7), MAX(2, L(2)), C_HL);
        text_center(PX(20), bx + ww / 2.f, yy + hh / 2, on ? C_HLT : C_TEXT, f->dbtns[i]);
    }
    g_alpha = 256;
}

static void draw_toast(void) {
    if (!*toast_text) return;
    long t = t_now - toast_t0, hold = 2000;
    if (!freeze && t > hold + 220) { toast_text[0] = 0; return; }
    float p = freeze || !motion ? 1 : t < 220 ? ease(t / 220.f) : t > hold ? 1 - ease((t - hold) / 220.f) : 1;
    if (!freeze && motion && (t < 220 || t > hold)) need_fast = 1;
    else if (!freeze) need_slow = 1; // (to take it away on time)
    int size = PX(19);
    float w = MIN(600.f, text_w(size, toast_text) / S + 64);
    float y = -44 + 54 * p;
    uint32_t c = !strcmp(toast_kind, "err") ? C_ERR : C_HL;
    int x = X(320 - w / 2);
    rrect(x, Y(y), L(w), L(36), L(10), with_a(C_BG, 0xF4));
    rrect_line(x, Y(y), L(w), L(36), L(10), MAX(2, L(2)), c);
    int cy = Y(y + 18);
    if (!strcmp(toast_kind, "ok")) icon_check(x + L(16), cy, L(13), c);
    else {
        disc(x + L(23), cy, L(9), c);
        text_center(PX(14), x + L(23), cy, C_BG, !strcmp(toast_kind, "err") ? "!" : "i");
    }
    clip(x + L(38), Y(y), L(w - 46), L(36));
    text(size, x + L(40), baseline(size, cy), C_TEXT, toast_text);
    noclip();
}

static void draw_letter(void) {
    if (!*letter) return;
    long t = t_now - letter_t0;
    if (!freeze && t > 650) { letter[0] = 0; return; }
    g_alpha = freeze || t < 450 ? 256 : (int)(256 * (1 - (t - 450) / 200.f));
    need_fast = !freeze;
    int x = X(265), y = Y(182), s = L(110);
    rrect(x, y, s, s, L(14), with_a(C_BG, 0xEC));
    rrect_line(x, y, s, s, L(14), MAX(2, L(2)), C_HL);
    text_center(PX(72), x + s / 2.f, y + s / 2, C_TEXT, letter);
    g_alpha = 256;
}

static long perf_t, perf_frames, perf_busy;
static int perf_fps, perf_ui, perf_cpu;
static unsigned long long cpu_tot, cpu_idle;

static void draw_perf(long busy_ms) {
    perf_frames++, perf_busy += busy_ms;
    if (t_now - perf_t >= 1000) {
        perf_fps = (int)(perf_frames * 1000 / MAX(1, t_now - perf_t));
        perf_ui = (int)(perf_busy * 100 / MAX(1, t_now - perf_t));
        FILE *f = fopen("/proc/stat", "r");
        unsigned long long v[8] = {0}, tot = 0;
        if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            for (int i = 0; i < 8; i++) tot += v[i];
            if (cpu_tot && tot > cpu_tot) perf_cpu = (int)(100 - (v[3] + v[4] - cpu_idle) * 100 / (tot - cpu_tot));
            cpu_tot = tot, cpu_idle = v[3] + v[4];
        }
        if (f) fclose(f);
        perf_t = t_now, perf_frames = perf_busy = 0;
    }
    char s[64];
    snprintf(s, sizeof s, "%d fps  ui %d%%  cpu %d%%", perf_fps, perf_ui, perf_cpu);
    text(PX(13), X(232), baseline(PX(13), Y(30)), with_a(C_DIM, 0xC0), s);
}

// ---------------------------------------------------------------- one screen

static void draw_scene(const Frame *f) {
    int kind = f->boot ? 0 : f->ntiles ? 1 : 2;
    draw_chrome(kind);
    noclip();
    if (f->boot) {
        draw_boot(f);
        return;
    }
    if (perf) draw_perf(0);
    draw_statusbar(f->statusbar);
    clip(X(10), Y(404), L(616), L(30));
    text(PX(base_size), X(22), baseline(PX(base_size), Y(419)), C_TEXT, f->status);
    noclip();
    draw_footer(f->footer);
    if (f->ntiles) draw_tiles(f);
    else if (f->cf) draw_coverflow(f);
    else if (f->osk) draw_osk(f);
    else if (f->busy) draw_busy(f);
    else {
        draw_list(f);
        draw_art(f);
    }
}

// The content area slides (push / pop) or fades between the last screen and this one
static void composite(float p) {
    int y0 = trans_full ? 0 : Y(64), y1 = trans_full ? H : Y(403);
    memcpy(outbuf, canvas, (size_t)W * H * 4);
    int a = (int)(256 * p);
    for (int y = y0; y < y1; y++) {
        uint32_t *o = outbuf + (size_t)y * W, *n = canvas + (size_t)y * W, *old = prevbuf + (size_t)y * W;
        if (!strcmp(trans_kind, "fade")) {
            for (int x = 0; x < W; x++) o[x] = mixc(old[x], n[x], a);
            continue;
        }
        int dir = !strcmp(trans_kind, "pop") ? 1 : -1, shift = (int)(W * p);
        for (int x = 0; x < W; x++) {
            int sx = x - dir * shift; // where this pixel was on the old screen
            int nx = x - dir * (shift - W);
            o[x] = (sx >= 0 && sx < W) ? old[sx] : (nx >= 0 && nx < W) ? n[nx] : 0xFF000000;
        }
    }
}

// Draws everything as it is at time now; returns when to draw again (0: not until
// something happens)
static long render(void) {
    long t0 = now_ms();
    t_now = t0;
    tick_no++;
    need_fast = need_slow = 0;
    if (!has_cur) return 0;
    // ease the moving parts toward where they belong
    float dt = MIN(100, t_now - last_draw);
    float k = freeze || !motion ? 1 : 1 - expf(-dt / 55.f);
    list_off += (cur.list_start - list_off) * k;
    hl_pos += (cur.list_start + cur.highlight - hl_pos) * k;
    cf_pos += (cur.cf_sel - cf_pos) * (freeze || !motion ? 1 : 1 - expf(-dt / 75.f));
    if (fabsf(list_off - cur.list_start) < .004f) list_off = cur.list_start;
    else need_fast = 1;
    if (fabsf(hl_pos - (cur.list_start + cur.highlight)) < .004f) hl_pos = cur.list_start + cur.highlight;
    else need_fast = 1;
    if (fabsf(cf_pos - cur.cf_sel) < .002f) cf_pos = cur.cf_sel;
    else need_fast = 1;

    noclip();
    draw_scene(&cur);
    if (dialog_on) {
        darken();
        draw_dialog(&dlg);
    }
    draw_letter();
    draw_toast();
    shown = canvas;
    if (*trans_kind) {
        float p = ease(anim(trans_t0, 210));
        if (p < 1) {
            composite(p);
            shown = outbuf;
        } else trans_kind[0] = 0;
    }
    last_draw = t_now;
    if (perf) perf_busy += now_ms() - t0;
    int dl = strstr(cur.statusbar, "{DL}") != NULL;
    for (int i = 0; i < cur.ntiles; i++) dl |= !strcmp(cur.tiles[i].sub, "{DL}");
    if (need_fast) return t_now + 16;
    if (need_slow || perf) return t_now + 33;
    if (dl) return t_now + 500;
    return 0;
}

// A whole frame has arrived
static void commit(void) {
    Frame *f = &incoming;
    if (f->dialog) {
        if (!dialog_on) dialog_t0 = now_ms();
        dlg = *f;
        dialog_on = 1;
        if (!has_cur) { cur = *f; cur.dialog = 0; has_cur = 1; }
        return;
    }
    int was_dialog = dialog_on;
    dialog_on = 0;
    pic_drop_queue();
    long t = now_ms();
    if (has_cur && motion && *f->transition && !was_dialog && shown) {
        COPY(trans_kind, f->transition);
        trans_full = cur.boot || f->boot;
        memcpy(prevbuf, shown, (size_t)W * H * 4);
        trans_t0 = t;
    }
    last = cur;
    // the list slides only within the same list, by less than a screen
    int same_list = has_cur && *f->list_key && !strcmp(f->list_key, last.list_key) && !*f->transition;
    if (!same_list || fabsf(f->list_start - list_off) > MAX(f->nitems, 1) || !motion) list_off = f->list_start;
    if (!same_list || f->highlight < 0 || last.highlight < 0 || fabsf(f->list_start + f->highlight - hl_pos) > MAX(f->nitems, 1) || !motion)
        hl_pos = f->list_start + f->highlight;
    if (!f->cf || !has_cur || strcmp(f->cf_key, last.cf_key) || !motion) cf_pos = f->cf_sel;
    else if (fabsf(f->cf_sel - cf_pos) > 6) cf_pos = f->cf_sel + (f->cf_sel > cf_pos ? -6 : 6);
    if (!has_cur || strcmp(f->cf_key, last.cf_key) || f->cf_sel != last.cf_sel || f->highlight != last.highlight) t_frame = t;
    if (f->boot && !last.boot) boot_shown = 0;
    if (!last.ntiles || !has_cur)
        for (int i = 0; i < 6; i++) tile_glow[i] = i == f->tile_sel;
    cur = *f;
    has_cur = 1;
}

// A line from launch.sh. Returns 0 on QUIT.
static int handle_line(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    if (!strcmp(s, "END")) {
        commit();
        frame_reset(&incoming);
        return 2;
    }
    if (!strcmp(s, "QUIT")) return 0;
    if (!strcmp(s, "FLUSH_CACHE")) {
        pic_flush();
        return 2;
    }
    char *c = strchr(s, ':');
    if (!c) return 1;
    *c = 0;
    char *v = c + 1;
    if (!strcmp(s, "TOAST")) {
        char *k = strchr(v, ':');
        if (k) *k = 0;
        COPY(toast_kind, v);
        COPY(toast_text, k ? k + 1 : "");
        toast_t0 = now_ms();
        return 2;
    }
    if (!strcmp(s, "LETTER")) {
        COPY(letter, v);
        letter_t0 = now_ms();
        return 2;
    }
    parse(&incoming, s, v);
    return 1;
}

// ---------------------------------------------------------------- output

static int fb_flip;
#ifdef __linux__
static int fb_fd = -1, fb_page, fb_bpp;
static uint8_t *fb_mem;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
#endif
static const char *preview_out;

static int screen_open(void) {
#ifdef __linux__
    const char *dev = getenv("RMUI_FB");
    fb_fd = open(dev && *dev ? dev : "/dev/fb0", O_RDWR);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)) return 0;
    fb_bpp = vinfo.bits_per_pixel;
    if (fb_bpp != 32 && fb_bpp != 16) return 0;
    fb_mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) return 0;
    W = vinfo.xres, H = vinfo.yres;
    fb_page = vinfo.yres ? vinfo.yoffset / vinfo.yres : 0;
    // Page flipping (no tearing) when the card has room for two screens
    const char *nf = getenv("RMUI_NOFLIP");
    fb_flip = !(nf && *nf == '1') && vinfo.yres_virtual >= vinfo.yres * 2 &&
              finfo.smem_len >= (size_t)finfo.line_length * vinfo.yres * 2;
    return 1;
#else
    return 0;
#endif
}

static void present(const uint32_t *src) {
    if (preview_out) {
        unsigned char *rgb = malloc((size_t)W * H * 3);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            rgb[i * 3] = src[i] >> 16, rgb[i * 3 + 1] = src[i] >> 8, rgb[i * 3 + 2] = src[i];
        }
        stbi_write_png(preview_out, W, H, 3, rgb, W * 3);
        free(rgb);
        return;
    }
#ifdef __linux__
    int page = fb_flip ? !fb_page : fb_page;
    uint8_t *base = fb_mem + (size_t)page * vinfo.yres * finfo.line_length;
    int plain = fb_bpp == 32 && vinfo.red.offset == 16 && vinfo.green.offset == 8 && vinfo.blue.offset == 0;
    for (int y = 0; y < H; y++) {
        const uint32_t *s = src + (size_t)y * W;
        uint8_t *row = base + (size_t)y * finfo.line_length + (size_t)vinfo.xoffset * fb_bpp / 8;
        if (plain) {
            memcpy(row, s, (size_t)W * 4); // (canvas pixels are always opaque)
        } else if (fb_bpp == 32) {
            uint32_t *d = (uint32_t *)row;
            for (int x = 0; x < W; x++) {
                uint32_t p = s[x];
                d[x] = (p >> 16 & 255) << vinfo.red.offset | (p >> 8 & 255) << vinfo.green.offset |
                       (p & 255) << vinfo.blue.offset | (vinfo.transp.length ? 255u << vinfo.transp.offset : 0);
            }
        } else {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < W; x++) {
                uint32_t p = s[x];
                d[x] = (uint16_t)((p >> 16 & 255) >> (8 - vinfo.red.length) << vinfo.red.offset |
                                  (p >> 8 & 255) >> (8 - vinfo.green.length) << vinfo.green.offset |
                                  (p & 255) >> (8 - vinfo.blue.length) << vinfo.blue.offset);
            }
        }
    }
    if (fb_flip) {
        vinfo.yoffset = page * vinfo.yres;
        if (ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo)) {
            fprintf(stderr, "rmui: page flipping failed, drawing directly\n");
            fb_flip = 0;
            vinfo.yoffset = fb_page * vinfo.yres;
            present(src);
            return;
        }
        fb_page = page;
    }
#endif
}

// Leaves the screen on its first page, the way the next program expects it
static void screen_close(void) {
#ifdef __linux__
    if (fb_fd < 0 || !fb_mem || fb_mem == MAP_FAILED) return;
    if (fb_flip && fb_page != 0) {
        memcpy(fb_mem, fb_mem + (size_t)fb_page * vinfo.yres * finfo.line_length, (size_t)vinfo.yres * finfo.line_length);
        vinfo.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        fb_page = 0;
    }
#endif
}

static void screen_size(int w, int h) {
    W = w, H = h;
    S = MIN(W / 640.f, H / 480.f);
    OX = (int)((W - 640 * S) / 2), OY = (int)((H - 480 * S) / 2);
    canvas = calloc((size_t)W * H, 4);
    prevbuf = calloc((size_t)W * H, 4);
    outbuf = calloc((size_t)W * H, 4);
    noclip();
}

// ---------------------------------------------------------------- modes

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int feed_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    int any = 0;
    while (fgets(line, sizeof line, f)) any |= handle_line(line) == 2;
    fclose(f);
    if (!any) { commit(); frame_reset(&incoming); } // a frame file without END
    return 1;
}

static int run_daemon(const char *fifo) {
    int fd = open(fifo, O_RDWR | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "rmui: can't open %s: %s\n", fifo, strerror(errno)); return 1; }
    if (pipe(wake) == 0) {
        fcntl(wake[0], F_SETFL, O_NONBLOCK);
        fcntl(wake[1], F_SETFL, O_NONBLOCK);
    }
    const char *nt = getenv("RMUI_NOTHREAD");
    pthread_t th;
    threaded = !(nt && *nt == '1') && wake[0] >= 0 && pthread_create(&th, NULL, loader, NULL) == 0;
    static char buf[1 << 16];
    size_t len = 0;
    long next = 0;
    int dirty = 0;
    while (!stop) {
        long t = now_ms(), wait = dirty ? 0 : next ? MAX(0, next - t) : -1;
        struct pollfd p[2] = {{fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
        int n = poll(p, wake[0] >= 0 ? 2 : 1, (int)wait);
        if (stop) break;
        if (n > 0 && (p[1].revents & POLLIN)) {
            char junk[64];
            while (read(wake[0], junk, sizeof junk) > 0) {}
            dirty = 1;
        }
        if (n > 0 && (p[0].revents & POLLIN)) {
            ssize_t r;
            while ((r = read(fd, buf + len, sizeof buf - 1 - len)) > 0) {
                len += r;
                char *nl;
                while ((nl = memchr(buf, '\n', len))) {
                    *nl = 0;
                    int h = handle_line(buf);
                    if (!h) goto quit;
                    if (h == 2) dirty = 1;
                    len -= nl + 1 - buf;
                    memmove(buf, nl + 1, len);
                }
                if (len == sizeof buf - 1) len = 0;
            }
        }
        // draw when something changed or moves - but a whole frame at a time
        if (dirty || (next && now_ms() >= next)) {
            next = render();
            if (shown) present(shown);
            dirty = 0;
        }
    }
quit:
    screen_close();
    return 0;
}

static int run_thumb(char **a) {
    int w = atoi(a[2]), h = atoi(a[3]), iw, ih, n;
    if (w <= 0 || h <= 0 || !stbi_info(a[0], &iw, &ih, &n)) return 1;
    float k = MIN(1.f, MIN((float)w / iw, (float)h / ih));
    Img im = img_load(a[0], MAX(1, (int)(iw * k)), MAX(1, (int)(ih * k)), STRETCH);
    if (!im.px) return 1;
    unsigned char *rgb = malloc((size_t)im.w * im.h * 3);
    for (size_t i = 0; i < (size_t)im.w * im.h; i++) { // (premultiplied: over black)
        rgb[i * 3] = im.px[i] >> 16, rgb[i * 3 + 1] = im.px[i] >> 8, rgb[i * 3 + 2] = im.px[i];
    }
    return stbi_write_jpg(a[1], im.w, im.h, 3, rgb, 85) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--thumb")) return run_thumb(argv + 2);
    frame_reset(&incoming);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 6 && !strcmp(argv[1], "--preview")) {
        // draws the frames with everything at rest, saves the last screen
        screen_size(atoi(argv[2]), atoi(argv[3]));
        preview_out = argv[4];
        freeze = 1;
        for (int i = 5; i < argc; i++)
            if (!feed_file(argv[i])) { fprintf(stderr, "rmui: can't read %s\n", argv[i]); return 1; }
        render();
        if (shown) present(shown);
        return 0;
    }
    preview_out = getenv("RMUI_PREVIEW");
    if (preview_out && !*preview_out) preview_out = NULL;
    if (preview_out) screen_size(1024, 768);
    else if (screen_open()) screen_size(W, H);
    else { fprintf(stderr, "rmui: can't open the screen\n"); return 1; }
    if (argc == 3 && !strcmp(argv[1], "-d")) return run_daemon(argv[2]);
    if (argc == 2) { // one frame, then exit
        freeze = 1;
        if (!feed_file(argv[1])) return 1;
        render();
        fb_flip = 0;
        if (shown) present(shown);
        return 0;
    }
    fprintf(stderr, "usage: rmui -d FIFO | rmui FRAME | rmui --thumb IN OUT W H | rmui --preview W H OUT.png FRAME...\n");
    return 2;
}
