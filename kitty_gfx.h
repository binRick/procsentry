// Minimal kitty graphics protocol support for use alongside termpaint.
// SPDX-License-Identifier: 0BSD
#ifndef KITTY_GFX_H
#define KITTY_GFX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int fd;             // tty fd used for writes and size queries
    int cols, rows;     // terminal cell grid
    int px_w, px_h;     // terminal size in pixels
    int fb_w, fb_h;     // framebuffer size (px size capped to max_dim)
    int max_dim;
    uint8_t *fb;        // RGB24, fb_w * fb_h * 3
    uint32_t cur_id;    // id of the image currently on screen (0 = none)
} kitty_ctx;

// Probe the terminal for kitty graphics support. Speaks to the terminal in
// raw mode, so call this BEFORE termpaint takes over the tty.
bool kitty_detect(int fd);

bool kitty_init(kitty_ctx *k, int fd, int max_dim);
// Re-read the terminal size; (re)allocate the framebuffer if it changed.
// Returns false if no usable framebuffer exists.
bool kitty_sync_size(kitty_ctx *k);
// Transmit the framebuffer and display it scaled to the full cell grid,
// underneath the text layer. Replaces the previously presented frame.
void kitty_present(kitty_ctx *k);
void kitty_close(kitty_ctx *k);

// Synchronized output (DECSET 2026) so cell + image updates land atomically.
void kitty_begin_sync(int fd);
void kitty_end_sync(int fd);

#endif
