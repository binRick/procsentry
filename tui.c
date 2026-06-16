// Shared TUI toolkit — see tui.h. SPDX-License-Identifier: 0BSD
#include "tui.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kitty_gfx.h"

// ---------------------------------------------------------------- gfx state
#define TUI_MAX_COLS 1024
#define TUI_MAX_ROWS 320

static int tty_fd = -1;
static bool detected;
static bool pixel_mode;
static kitty_ctx kctx;

// cut-out mask: 1 where the app drew a panel/text this frame -> the wallpaper
// is punched transparent there so the cells show through.
static uint8_t mask[TUI_MAX_ROWS * TUI_MAX_COLS];
static int g_cols, g_rows;   // cell grid for the current frame

static void unpack(int c, int *r, int *g, int *b) {
    *r = (c >> 16) & 0xff;
    *g = (c >> 8) & 0xff;
    *b = c & 0xff;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void mark(int x, int y, int w, int h) {
    if (!pixel_mode) {
        return;
    }
    int x0 = clampi(x, 0, TUI_MAX_COLS);
    int y0 = clampi(y, 0, TUI_MAX_ROWS);
    int x1 = clampi(x + w, 0, TUI_MAX_COLS);
    int y1 = clampi(y + h, 0, TUI_MAX_ROWS);
    for (int yy = y0; yy < y1; yy++) {
        memset(&mask[yy * TUI_MAX_COLS + x0], 1, (size_t)(x1 - x0));
    }
}

// ---------------------------------------------------------------- lifecycle
bool tui_gfx_detect(const char *env_prefix) {
    char var[64];
    snprintf(var, sizeof(var), "%s_CELLS", env_prefix);
    if (getenv(var)) {
        return false;
    }
    tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) {
        return false;
    }
    detected = kitty_detect(tty_fd);
    return detected;
}

bool tui_gfx_start(const char *env_prefix) {
    if (!detected || tty_fd < 0) {
        return false;
    }
    int max_dim = 640;                 // a touch sharper than the effects' 512
    char var[64];
    snprintf(var, sizeof(var), "%s_MAXDIM", env_prefix);
    const char *s = getenv(var);
    if (s && atoi(s) >= 128) {
        max_dim = atoi(s);
    }
    pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    return pixel_mode;
}

bool tui_pixel_mode(void) { return pixel_mode; }
int  tui_fb_w(void) { return kctx.fb_w; }
int  tui_fb_h(void) { return kctx.fb_h; }

void tui_gfx_stop(void) {
    if (pixel_mode) {
        kitty_close(&kctx);
    }
    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
}

// ---------------------------------------------------------------- per-frame
void tui_frame_begin(termpaint_surface *s) {
    g_cols = termpaint_surface_width(s);
    g_rows = termpaint_surface_height(s);
    if (pixel_mode) {
        memset(mask, 0, sizeof(mask));
        kitty_begin_sync(tty_fd);
    }
}

void tui_frame_end(termpaint_surface *s, termpaint_terminal *term) {
    (void)s;
    termpaint_terminal_flush(term, false);
    if (pixel_mode) {
        kitty_present(&kctx);
        kitty_end_sync(tty_fd);
    }
}

// ---------------------------------------------------------------- backdrop
// fx,fy in [0,1]; writes 0..255 rgb for the chosen style.
static void back_pixel(int style, float fx, float fy, float t,
                       int r1, int g1, int b1, int r2, int g2, int b2,
                       int *or_, int *og, int *ob) {
    if (style == TUI_BACK_PLASMA) {
        float v = sinf(fx * 9.0f + t)
                + sinf(fy * 7.0f - t * 0.8f)
                + sinf((fx + fy) * 6.0f + t * 0.6f)
                + sinf(hypotf(fx - 0.5f, fy - 0.5f) * 16.0f - t * 1.4f);
        float n = (v + 4.0f) / 8.0f;             // 0..1
        float hot = n * n * n;                   // sparkle the crests
        *or_ = (int)(r1 + (r2 - r1) * n + hot * 60);
        *og  = (int)(g1 + (g2 - g1) * n + hot * 60);
        *ob  = (int)(b1 + (b2 - b1) * n + hot * 60);
    } else if (style == TUI_BACK_GRID) {
        // dim base, scrolling glow grid, soft vignette
        float base = 0.18f;
        float gx = fx * 46.0f, gy = fy * 30.0f - t * 1.5f;
        float lx = fabsf(gx - floorf(gx) - 0.5f);
        float ly = fabsf(gy - floorf(gy) - 0.5f);
        float line = expf(-lx * lx * 120.0f) + expf(-ly * ly * 120.0f);
        float vig = 1.0f - 0.7f * (fabsf(fx - 0.5f) + fabsf(fy - 0.5f));
        if (vig < 0) vig = 0;
        *or_ = (int)((r1 * base + r2 * line * 0.5f) * vig);
        *og  = (int)((g1 * base + g2 * line * 0.5f) * vig);
        *ob  = (int)((b1 * base + b2 * line * 0.5f) * vig);
    } else {                                     // TUI_BACK_AURORA
        float shimmer = 0.10f * sinf(fx * 9.4f + t * 0.7f)
                      + 0.07f * sinf(fy * 12.6f - t * 0.5f);
        float g = fy + shimmer;
        if (g < 0) g = 0;
        if (g > 1) g = 1;
        float ribbon_y = 0.42f + 0.16f * sinf(fx * 3.0f + t * 0.6f);
        float d = fy - ribbon_y;
        float rib = expf(-d * d * 55.0f);        // moving light band
        *or_ = (int)(r1 + (r2 - r1) * g + rib * (235 - (r1 + r2) * 0.5f));
        *og  = (int)(g1 + (g2 - g1) * g + rib * (235 - (g1 + g2) * 0.5f));
        *ob  = (int)(b1 + (b2 - b1) * g + rib * (235 - (b1 + b2) * 0.5f));
    }
}

void tui_backdrop(int style, double t, int c1, int c2) {
    if (!pixel_mode || !kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    int cols = kctx.cols < TUI_MAX_COLS ? kctx.cols : TUI_MAX_COLS;
    int rows = kctx.rows < TUI_MAX_ROWS ? kctx.rows : TUI_MAX_ROWS;
    uint8_t *fb = kctx.fb;
    int r1, g1, b1, r2, g2, b2;
    unpack(c1, &r1, &g1, &b1);
    unpack(c2, &r2, &g2, &b2);
    float tf = (float)t;

    for (int py = 0; py < H; py++) {
        int crow = cols > 0 ? py * rows / H : 0;
        float fy = (float)py / (float)H;
        uint8_t *row = &fb[(size_t)py * W * 4];
        const uint8_t *mrow = &mask[crow * TUI_MAX_COLS];
        for (int px = 0; px < W; px++) {
            uint8_t *p = &row[px * 4];
            int ccol = px * cols / W;
            if (mrow[ccol]) {                    // panel here -> transparent
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            int r, g, b;
            back_pixel(style, (float)px / (float)W, fy, tf,
                       r1, g1, b1, r2, g2, b2, &r, &g, &b);
            p[0] = (uint8_t)clampi(r, 0, 255);
            p[1] = (uint8_t)clampi(g, 0, 255);
            p[2] = (uint8_t)clampi(b, 0, 255);
            p[3] = 255;
        }
    }
}

void tui_background(termpaint_surface *s, int style, double t, int c1, int c2) {
    (void)style;
    if (pixel_mode) {
        termpaint_surface_clear(s, TUI_TEXT, TUI_BG);
        return;
    }
    // cell mode: a quiet, dark vertical gradient so the UI stays readable.
    // (the lively animated wallpaper is the kitty pixel mode's job.)
    int w = termpaint_surface_width(s);
    int h = termpaint_surface_height(s);
    int r1, g1, b1, r2, g2, b2;
    unpack(c1, &r1, &g1, &b1);
    unpack(c2, &r2, &g2, &b2);
    for (int y = 0; y < h; y++) {
        float fy = h > 1 ? (float)y / (float)(h - 1) : 0;
        // dim it heavily and lift off pure black a touch
        int r = clampi((int)((r1 + (r2 - r1) * fy) * 0.18f) + 4, 0, 255);
        int g = clampi((int)((g1 + (g2 - g1) * fy) * 0.18f) + 5, 0, 255);
        int b = clampi((int)((b1 + (b2 - b1) * fy) * 0.18f) + 7, 0, 255);
        int bg = TERMPAINT_RGB_COLOR(r, g, b);
        for (int x = 0; x < w; x++) {
            const char *ch = " ";
            // a few faint drifting motes so it isn't dead flat
            int hsh = (x * 73856093) ^ (y * 19349663) ^ ((int)(t * 1.5) * 83492791);
            if ((hsh & 2047) < 2) {
                ch = "·";
            }
            termpaint_surface_write_with_colors(s, x, y, ch, TUI_FAINT, bg);
        }
    }
}

// ---------------------------------------------------------------- widgets
int tui_strwidth(const char *str) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if ((*p & 0xc0) != 0x80) {                // count non-continuation bytes
            n++;
        }
    }
    return n;
}

void tui_fill(termpaint_surface *s, int x, int y, int w, int h, int bg) {
    int sw = termpaint_surface_width(s), sh = termpaint_surface_height(s);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= sh) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= sw) {
                continue;
            }
            termpaint_surface_write_with_colors(s, xx, yy, " ", bg, bg);
        }
    }
    mark(x, y, w, h);
}

int tui_text(termpaint_surface *s, int x, int y, const char *str, int fg, int bg) {
    int sw = termpaint_surface_width(s), sh = termpaint_surface_height(s);
    if (y < 0 || y >= sh || x >= sw) {
        return 0;
    }
    int width = tui_strwidth(str);
    termpaint_surface_write_with_colors(s, x, y, str, fg, bg);
    int drawn = width;
    if (x + drawn > sw) {
        drawn = sw - x;
    }
    if (x < 0) {
        drawn += x;
    }
    mark(x < 0 ? 0 : x, y, drawn, 1);
    return width;
}

void tui_text_clip(termpaint_surface *s, int x, int y, int maxw,
                   const char *str, int fg, int bg) {
    if (maxw <= 0) {
        return;
    }
    if (tui_strwidth(str) <= maxw) {
        tui_text(s, x, y, str, fg, bg);
        return;
    }
    // copy up to maxw-1 display columns, then an ellipsis
    char tmp[512];
    int cols = 0;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)str;
         *p && cols < maxw - 1 && o < sizeof(tmp) - 4; p++) {
        if ((*p & 0xc0) != 0x80) {
            if (cols >= maxw - 1) {
                break;
            }
            cols++;
        }
        tmp[o++] = (char)*p;
    }
    tmp[o] = 0;
    tui_text(s, x, y, tmp, fg, bg);
    tui_text(s, x + cols, y, "…", fg, bg);
}

void tui_text_pad(termpaint_surface *s, int x, int y, int w,
                  const char *str, int fg, int bg) {
    tui_fill(s, x, y, w, 1, bg);
    tui_text_clip(s, x, y, w, str, fg, bg);
}

void tui_hline(termpaint_surface *s, int x, int y, int w, int fg, int bg) {
    for (int i = 0; i < w; i++) {
        tui_text(s, x + i, y, "─", fg, bg);
    }
}

void tui_panel(termpaint_surface *s, int x, int y, int w, int h,
               const char *title, int border_fg, int title_fg, int bg) {
    if (w < 2 || h < 2) {
        return;
    }
    tui_fill(s, x, y, w, h, bg);                  // interior + mask
    tui_text(s, x, y, "╭", border_fg, bg);
    tui_text(s, x + w - 1, y, "╮", border_fg, bg);
    tui_text(s, x, y + h - 1, "╰", border_fg, bg);
    tui_text(s, x + w - 1, y + h - 1, "╯", border_fg, bg);
    for (int i = 1; i < w - 1; i++) {
        tui_text(s, x + i, y, "─", border_fg, bg);
        tui_text(s, x + i, y + h - 1, "─", border_fg, bg);
    }
    for (int i = 1; i < h - 1; i++) {
        tui_text(s, x, y + i, "│", border_fg, bg);
        tui_text(s, x + w - 1, y + i, "│", border_fg, bg);
    }
    if (title && *title) {
        char t[256];
        snprintf(t, sizeof(t), " %s ", title);
        int tw = tui_strwidth(t);
        if (tw > w - 2) {
            tw = w - 2;
        }
        tui_text_clip(s, x + 2, y, w - 4, t, title_fg, bg);
    }
}

// ---------------------------------------------------------------- input
void tui_input_reset(tui_input *in) {
    in->buf[0] = 0;
    in->len = 0;
}

void tui_input_set(tui_input *in, const char *s) {
    snprintf(in->buf, sizeof(in->buf), "%s", s ? s : "");
    in->len = (int)strlen(in->buf);
}

void tui_input_char(tui_input *in, const char *utf8, unsigned len) {
    if (len == 0 || in->len + (int)len >= (int)sizeof(in->buf) - 1) {
        return;
    }
    // ignore control bytes
    if (len == 1 && (unsigned char)utf8[0] < 0x20) {
        return;
    }
    memcpy(in->buf + in->len, utf8, len);
    in->len += (int)len;
    in->buf[in->len] = 0;
}

void tui_input_backspace(tui_input *in) {
    if (in->len == 0) {
        return;
    }
    int i = in->len - 1;
    while (i > 0 && ((unsigned char)in->buf[i] & 0xc0) == 0x80) {
        i--;                                      // step back over UTF-8 tail
    }
    in->buf[i] = 0;
    in->len = i;
}
