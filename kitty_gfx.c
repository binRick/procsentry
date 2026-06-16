// SPDX-License-Identifier: 0BSD
#include "kitty_gfx.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, buf, len);
        if (r < 0) {
            return;
        }
        buf += r;
        len -= (size_t)r;
    }
}

bool kitty_detect(int fd) {
    if (!isatty(fd)) {
        return false;
    }
    struct termios saved, raw;
    if (tcgetattr(fd, &saved) != 0) {
        return false;
    }
    raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0) {
        return false;
    }

    // Ask the terminal to validate (not display) a tiny image, then send
    // DA1 as a sentinel every terminal answers. Graphics support confirmed
    // if the _G reply arrives before the DA1 reply.
    const char *probe = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c";
    write_all(fd, probe, strlen(probe));

    char buf[1024];
    size_t n = 0;
    bool ok = false;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 1000) <= 0) {
            break;
        }
        ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        buf[n] = 0;
        if (strstr(buf, "_Gi=31;OK")) {
            ok = true;
        }
        const char *da1 = strstr(buf, "\x1b[?");
        if (da1 && strchr(da1, 'c')) {
            break;
        }
        if (n >= sizeof(buf) - 1) {
            break;
        }
    }
    tcsetattr(fd, TCSAFLUSH, &saved);
    return ok;
}

bool kitty_sync_size(kitty_ctx *k) {
    struct winsize ws;
    if (ioctl(k->fd, TIOCGWINSZ, &ws) != 0
            || ws.ws_col == 0 || ws.ws_row == 0
            || ws.ws_xpixel == 0 || ws.ws_ypixel == 0) {
        return k->fb != NULL;
    }
    int fw = ws.ws_xpixel;
    int fh = ws.ws_ypixel;
    int m = fw > fh ? fw : fh;
    if (m > k->max_dim) {
        fw = fw * k->max_dim / m;
        fh = fh * k->max_dim / m;
    }
    if (k->fb && fw == k->fb_w && fh == k->fb_h
            && ws.ws_col == k->cols && ws.ws_row == k->rows) {
        return true;
    }
    free(k->fb);
    k->fb = calloc((size_t)fw * fh, 4);   // RGBA
    k->fb_w = fw;
    k->fb_h = fh;
    k->cols = ws.ws_col;
    k->rows = ws.ws_row;
    k->px_w = ws.ws_xpixel;
    k->px_h = ws.ws_ypixel;
    return k->fb != NULL;
}

bool kitty_init(kitty_ctx *k, int fd, int max_dim) {
    memset(k, 0, sizeof(*k));
    k->fd = fd;
    k->max_dim = max_dim;
    return kitty_sync_size(k) && k->fb != NULL;
}

static size_t b64emit(char *dst, const uint8_t *src, size_t len) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *d = dst;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i+1] << 8 | src[i+2];
        *d++ = t[v >> 18];
        *d++ = t[(v >> 12) & 63];
        *d++ = t[(v >> 6) & 63];
        *d++ = t[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16
                   | (i + 1 < len ? (uint32_t)src[i+1] << 8 : 0);
        *d++ = t[v >> 18];
        *d++ = t[(v >> 12) & 63];
        *d++ = (i + 1 < len) ? t[(v >> 6) & 63] : '=';
        *d++ = '=';
    }
    return (size_t)(d - dst);
}

void kitty_present(kitty_ctx *k) {
    if (!k->fb) {
        return;
    }
    // 3072 raw bytes → 4096 base64 chars per chunk; 3072/4 = 768 RGBA pixels exactly
    const size_t chunk_raw = 3072;
    size_t raw = (size_t)k->fb_w * k->fb_h * 4;    // RGBA
    size_t chunks = (raw + chunk_raw - 1) / chunk_raw;
    char *out = malloc(((raw + 2) / 3) * 4 + chunks * 128 + 256);
    if (!out) {
        return;
    }

    uint32_t id = (k->cur_id == 10) ? 11 : 10;
    char *p = out;
    p += sprintf(p, "\x1b" "7\x1b[H");
    size_t off = 0;
    bool first = true;
    while (off < raw) {
        size_t take = (raw - off < chunk_raw) ? (raw - off) : chunk_raw;
        int more = (off + take < raw) ? 1 : 0;
        if (first) {
            // f=32 RGBA, z=1 image on top of cells, C=1 cursor doesn't advance
            p += sprintf(p, "\x1b_Ga=T,q=2,i=%u,f=32,s=%d,v=%d,c=%d,r=%d,z=1,C=1,m=%d;",
                         id, k->fb_w, k->fb_h, k->cols, k->rows, more);
            first = false;
        } else {
            p += sprintf(p, "\x1b_Gq=2,m=%d;", more);
        }
        p += b64emit(p, k->fb + off, take);
        p += sprintf(p, "\x1b\\");
        off += take;
    }
    if (k->cur_id) {
        p += sprintf(p, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", k->cur_id);
    }
    p += sprintf(p, "\x1b" "8");
    write_all(k->fd, out, (size_t)(p - out));
    free(out);
    k->cur_id = id;
}

void kitty_close(kitty_ctx *k) {
    if (k->fd >= 0) {
        const char *del = "\x1b_Ga=d,d=A,q=2\x1b\\";
        write_all(k->fd, del, strlen(del));
    }
    free(k->fb);
    k->fb = NULL;
}

void kitty_begin_sync(int fd) {
    write_all(fd, "\x1b[?2026h", 8);
}

void kitty_end_sync(int fd) {
    write_all(fd, "\x1b[?2026l", 8);
}
