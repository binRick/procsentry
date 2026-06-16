// Shared TUI toolkit for the termfun menu-driven apps.
//
// Everything is built on termpaint cells. On kitty-graphics terminals the
// apps additionally float over an animated pixel "wallpaper": the toolkit
// fills a framebuffer everywhere the app did NOT draw a panel (a per-cell
// cut-out mask), and presents it with alpha so the cells punch through. The
// kitty image sits on top of the cells (z=1, see kitty_gfx.c), so the mask is
// what makes the backdrop read as if it were behind the UI.
//
// SPDX-License-Identifier: 0BSD
#ifndef TUI_H
#define TUI_H

#include <stdbool.h>
#include <stdint.h>

#include "termpaint.h"

// ---- base palette (GitHub-dark-ish; each app adds its own accent) ----
#define TUI_BLACK  TERMPAINT_RGB_COLOR(0, 0, 0)
#define TUI_BG     TERMPAINT_RGB_COLOR(13, 17, 23)
#define TUI_PANEL  TERMPAINT_RGB_COLOR(22, 27, 34)
#define TUI_PANEL2 TERMPAINT_RGB_COLOR(31, 38, 48)
#define TUI_BORDER TERMPAINT_RGB_COLOR(48, 54, 61)
#define TUI_TEXT   TERMPAINT_RGB_COLOR(201, 209, 217)
#define TUI_DIM    TERMPAINT_RGB_COLOR(139, 148, 158)
#define TUI_FAINT  TERMPAINT_RGB_COLOR(86, 95, 110)
#define TUI_WHITE  TERMPAINT_RGB_COLOR(246, 248, 250)
#define TUI_OK     TERMPAINT_RGB_COLOR(63, 185, 80)
#define TUI_WARN   TERMPAINT_RGB_COLOR(210, 153, 34)
#define TUI_BAD    TERMPAINT_RGB_COLOR(248, 81, 73)

// animated backdrop styles
enum { TUI_BACK_AURORA, TUI_BACK_PLASMA, TUI_BACK_GRID };

// ---- gfx lifecycle ----
// Call BEFORE termpaint takes over the tty (kitty detection needs raw mode).
// Honors <PREFIX>_CELLS to force pure cell rendering. Returns true if a kitty
// pixel backdrop is available.
bool tui_gfx_detect(const char *env_prefix);
// Call AFTER termpaint setup. Allocates the framebuffer; honors
// <PREFIX>_MAXDIM. Returns true if pixel mode is active.
bool tui_gfx_start(const char *env_prefix);
bool tui_pixel_mode(void);
int  tui_fb_w(void);
int  tui_fb_h(void);
void tui_gfx_stop(void);

// ---- per-frame ----
// Reset the cut-out mask and (pixel mode) open a synchronized-output block.
void tui_frame_begin(termpaint_surface *s);
// Paint the screen background: an animated cell gradient in cell mode, or a
// cleared surface in pixel mode (the image supplies the wallpaper).
void tui_background(termpaint_surface *s, int style, double t, int c1, int c2);
// Fill the framebuffer with the animated wallpaper wherever no panel/text was
// drawn this frame (pixel mode only; a no-op in cell mode).
void tui_backdrop(int style, double t, int c1, int c2);
// Flush the cells and (pixel mode) present the image + close the sync block.
void tui_frame_end(termpaint_surface *s, termpaint_terminal *term);

// ---- widgets (everything written marks the cut-out mask) ----
void tui_fill(termpaint_surface *s, int x, int y, int w, int h, int bg);
// Rounded panel with an optional title in the top border.
void tui_panel(termpaint_surface *s, int x, int y, int w, int h,
               const char *title, int border_fg, int title_fg, int bg);
// Horizontal rule made of light box-drawing.
void tui_hline(termpaint_surface *s, int x, int y, int w, int fg, int bg);
// Write a string; returns the display width written. Clips to the surface.
int  tui_text(termpaint_surface *s, int x, int y, const char *str, int fg, int bg);
// Write a string truncated with an ellipsis to fit maxw columns.
void tui_text_clip(termpaint_surface *s, int x, int y, int maxw,
                   const char *str, int fg, int bg);
// Write a string left-justified and padded with the bg colour to w columns
// (truncates with an ellipsis when too long). Handy for selectable rows.
void tui_text_pad(termpaint_surface *s, int x, int y, int w,
                  const char *str, int fg, int bg);
// Display width (columns) of a UTF-8 string.
int  tui_strwidth(const char *str);

// ---- minimal single-line text input (append + backspace) ----
typedef struct {
    char buf[256];
    int len;     // bytes used
} tui_input;
void tui_input_reset(tui_input *in);
void tui_input_set(tui_input *in, const char *s);
void tui_input_char(tui_input *in, const char *utf8, unsigned len);
void tui_input_backspace(tui_input *in);

#endif
