// procsentry — pick processes, then watch every program they exec, built on
// termpaint. A two-pane workflow: a filterable, multi-select process list,
// then a live tagged stream of exec() events captured from one `extrace`
// process per selected PID. Panels float over an animated kitty-graphics
// backdrop where supported (cells everywhere else).
//
// `extrace` (https://github.com/chneukirchen/extrace) traces program
// executions via the Linux proc connector; this app is a front-end for it, so
// the trace pane is Linux-only (it shows extrace's own error otherwise). The
// process picker works anywhere `ps` does.
//
// The picker is a `ps --forest` tree: just start typing to search (no mode to
// enter), and the search is subtree-aware — a match pulls in everything running
// under it, so typing `sshd` shows sshd and its child processes. Launch straight
// into a search with `procsentry sshd` (or PROCSENTRY_FILTER=sshd).
//
// SPDX-License-Identifier: 0BSD
//
//   SELECT:  type to search · ↑/↓ move (hold = faster) · Space/click select ·
//            wheel scroll · Enter trace · Esc clear search / quit
//   TRACE:   ↑/↓ or wheel scroll · PgUp/PgDn · f follow · b back · q quit
//
// Build: see Makefile. procsentry = cells; procsentry-gfx = kitty backdrop.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "tui.h"

#define MAXPROC   4096
#define MAXSEL    16
#define LINE_MAX  512
#define SCROLL_MAX 4000

#define ACCENT  TERMPAINT_RGB_COLOR(255, 176, 59)    // amber
#define SEL_BG  TERMPAINT_RGB_COLOR(120, 74, 12)     // dark amber row highlight
#define BACK1   TERMPAINT_RGB_COLOR(28, 14, 34)      // deep purple (top)
#define BACK2   TERMPAINT_RGB_COLOR(150, 48, 120)    // magenta (bottom)

// distinct, dark-background-readable colours, one per traced root
static const unsigned ROOT_COLORS[8] = {
    TERMPAINT_RGB_COLOR(255, 176,  59),
    TERMPAINT_RGB_COLOR( 88, 200, 160),
    TERMPAINT_RGB_COLOR(120, 170, 255),
    TERMPAINT_RGB_COLOR(244, 114, 182),
    TERMPAINT_RGB_COLOR(163, 113, 247),
    TERMPAINT_RGB_COLOR(126, 231, 135),
    TERMPAINT_RGB_COLOR(255, 123, 114),
    TERMPAINT_RGB_COLOR(224, 196, 108),
};

typedef struct {
    int pid;
    double cpu;
    int indent;          // leading-space count of the --forest line (tree depth)
    char user[24];
    char cmd[256];
} proc;

// one running `extrace -p PID` and its capture state
typedef struct {
    int pid;
    char name[48];
    unsigned color;
    pid_t child;
    int fd;                 // read end of the pipe, or -1
    bool ended;
    long count;             // exec events captured
    char partial[LINE_MAX]; // bytes of the line being assembled
    int plen;
} root;

enum { LL_EXEC, LL_HEADER, LL_NOTICE };

typedef struct {
    uint8_t root;
    uint8_t kind;       // LL_EXEC / LL_HEADER / LL_NOTICE
    uint16_t depth;     // tree depth for LL_EXEC (0 = direct child of the root)
    char text[LINE_MAX];
} logline;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static bool quit_requested;
static double anim_t;
static char hostname[128] = "localhost";

// ---- process picker ----
static proc procs[MAXPROC];
static int nprocs;
static int view[MAXPROC];           // indices into procs[] passing the filter
static int nview;
static int hi;                      // highlighted row (index into view[])
static int top;                     // first visible view row
static int list_rows = 1;
static int pick_x0, pick_pw, pick_ly0;   // picker list geometry, for mouse hit-testing
static double last_nav_t;           // accel: app time of the last up/down keypress
static int nav_streak;              // accel: consecutive fast up/down presses
static double since_refresh = 1e9;   // force immediate first sample
static const double refresh_interval = 3.0;

static int sel_pid[MAXSEL];
static char sel_name[MAXSEL][48];
static int nsel;

static tui_input filter;        // live search; the picker always type-to-filters

// ---- trace pane ----
static enum { PHASE_SELECT, PHASE_TRACE } phase = PHASE_SELECT;
static root roots[MAXSEL];
static int nroots;
static logline ring[SCROLL_MAX];
static long produced;               // total lines ever pushed
static int last_root = -1;          // root of the previous pushed line (header logic)
static bool follow = true;
static long view_bottom;            // logical line index shown at the bottom

// ---------------------------------------------------------------- selection
// Skip a leading `ps --forest` tree prefix ("  \_ ") so a selected process's
// label and trace header show the bare command, not the picker's tree art.
static const char *cmd_body(const char *s) {
    while (*s == ' ' || *s == '\\' || *s == '_') {
        s++;
    }
    return s;
}

// Copy a (possibly long) command into a short fixed label, truncating cleanly.
static void label_copy(char *dst, size_t n, const char *src) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    for (; src[i] && i < n - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static int sel_index(int pid) {
    for (int i = 0; i < nsel; i++) {
        if (sel_pid[i] == pid) {
            return i;
        }
    }
    return -1;
}

static void toggle_sel(const proc *p) {
    int idx = sel_index(p->pid);
    if (idx >= 0) {
        memmove(&sel_pid[idx], &sel_pid[idx + 1], (size_t)(nsel - idx - 1) * sizeof(int));
        memmove(&sel_name[idx], &sel_name[idx + 1], (size_t)(nsel - idx - 1) * sizeof(sel_name[0]));
        nsel--;
        return;
    }
    if (nsel >= MAXSEL) {
        return;
    }
    sel_pid[nsel] = p->pid;
    label_copy(sel_name[nsel], sizeof(sel_name[0]), cmd_body(p->cmd));
    nsel++;
}

// ---------------------------------------------------------------- sampling
static const char *ps_cmd(void) {
#ifdef __APPLE__
    // macOS ps has no --forest, so fall back to a flat, CPU-sorted list.
    return "ps -axo pid=,user=,pcpu=,args= -r 2>/dev/null";
#else
    // --forest draws a `\_` process tree into the args column (and orders by
    // it), so the picker shows parents and children the way `ps auxf` does.
    return "ps -eo pid=,user=,pcpu=,args= --forest 2>/dev/null";
#endif
}

static void sample_procs(void) {
    nprocs = 0;
    FILE *f = popen(ps_cmd(), "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (nprocs < MAXPROC && fgets(line, sizeof(line), f)) {
        proc p;
        memset(&p, 0, sizeof(p));
        int off = 0;
        // read pid/user/cpu, then keep the args field verbatim from the single
        // column separator on — preserving the --forest indentation.
        if (sscanf(line, "%d %23s %lf%n", &p.pid, p.user, &p.cpu, &off) >= 3 && off > 0) {
            const char *a = line + off;
            if (*a == ' ') {
                a++;
            }
            snprintf(p.cmd, sizeof(p.cmd), "%s", a);
            size_t n = strlen(p.cmd);
            while (n && (p.cmd[n - 1] == '\n' || p.cmd[n - 1] == '\r')) {
                p.cmd[--n] = 0;
            }
            while (p.cmd[p.indent] == ' ') {     // --forest depth = leading spaces
                p.indent++;
            }
            if (p.pid > 0 && p.cmd[0]) {
                procs[nprocs++] = p;
            }
        }
    }
    pclose(f);
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) {
        return true;
    }
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

static bool proc_matches(const proc *p) {
    if (!filter.len) {
        return true;
    }
    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", p->pid);
    return ci_contains(p->cmd, filter.buf) ||
           ci_contains(p->user, filter.buf) ||
           ci_contains(pidbuf, filter.buf);
}

static void rebuild_view(void) {
    int prev_pid = (hi >= 0 && hi < nview) ? procs[view[hi]].pid : -1;
    nview = 0;
    if (!filter.len) {
        for (int i = 0; i < nprocs; i++) {
            view[nview++] = i;
        }
    } else {
        // A match pulls in its whole --forest subtree: in DFS pre-order a node's
        // descendants are exactly the run of following procs indented deeper
        // than it. So "sshd" shows sshd *and* everything running under it.
        static bool keep[MAXPROC];
        memset(keep, 0, (size_t)nprocs * sizeof(keep[0]));
        for (int i = 0; i < nprocs; i++) {
            if (proc_matches(&procs[i])) {
                keep[i] = true;
                for (int j = i + 1; j < nprocs && procs[j].indent > procs[i].indent; j++) {
                    keep[j] = true;
                }
            }
        }
        for (int i = 0; i < nprocs; i++) {
            if (keep[i]) {
                view[nview++] = i;
            }
        }
    }
    // keep the highlight on the same pid across refresh/filter when possible
    hi = 0;
    if (prev_pid >= 0) {
        for (int i = 0; i < nview; i++) {
            if (procs[view[i]].pid == prev_pid) {
                hi = i;
                break;
            }
        }
    }
    if (hi >= nview) {
        hi = nview - 1;
    }
    if (hi < 0) {
        hi = 0;
    }
}

// ---------------------------------------------------------------- trace I/O
static void push_raw(int r, int kind, int depth, const char *text) {
    logline *L = &ring[produced % SCROLL_MAX];
    L->root = (uint8_t)r;
    L->kind = (uint8_t)kind;
    L->depth = (uint16_t)depth;
    snprintf(L->text, sizeof(L->text), "%s", text);
    produced++;
}

// A header line announces the traced (selected) process as the parent, so the
// exec lines that follow read as its subtree. Re-emitted whenever the active
// root changes, the way `tail -f` reprints a filename when the source switches.
static void push_header(int r) {
    char hdr[LINE_MAX];
    snprintf(hdr, sizeof(hdr), "%d  %s", roots[r].pid, roots[r].name);
    push_raw(r, LL_HEADER, 0, hdr);
    last_root = r;
}

// Push one captured extrace line for root r, prepending a parent header when
// the stream switches roots. extrace indents descendants by two spaces per
// level (direct children at column 0), and prefixes diagnostics with "extrace:".
static void push_line(int r, const char *raw) {
    int spaces = 0;
    while (raw[spaces] == ' ') {
        spaces++;
    }
    const char *body = raw + spaces;
    if (!*body) {
        return;
    }
    bool is_node = (body[0] >= '0' && body[0] <= '9');
    if (!is_node) {
        // extrace diagnostics — drop the routine noise nobody wants to see
        // ("process vanished before notification", "out of order message ...").
        if (strstr(body, "vanished") || strstr(body, "out of order")) {
            return;
        }
    }
    if (r != last_root) {
        push_header(r);
    }
    if (!is_node) {                             // a real diagnostic, not a tree node
        push_raw(r, LL_NOTICE, 0, body);
    } else {
        push_raw(r, LL_EXEC, spaces / 2, body);
        if (r >= 0 && r < nroots) {
            roots[r].count++;
        }
    }
}

static const char *extrace_bin(void) {
    const char *b = getenv("PROCSENTRY_BIN");
    return (b && *b) ? b : "extrace";
}

// fork+exec `extrace -p PID`, capturing its stdout+stderr on a non-blocking pipe.
static bool spawn_extrace(root *r) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {                       // child
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        for (int fd = 3; fd < 64; fd++) { // drop termpaint/kitty fds
            close(fd);
        }
        char pidbuf[16];
        snprintf(pidbuf, sizeof(pidbuf), "%d", r->pid);
        const char *bin = extrace_bin();
        char *argv[] = { (char *)bin, (char *)"-p", pidbuf, NULL };
        execvp(bin, argv);
        dprintf(STDERR_FILENO, "cannot run %s: %s\n", bin, strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    r->child = pid;
    r->fd = fds[0];
    r->ended = false;
    return true;
}

static void start_trace(void) {
    if (nsel == 0) {
        return;
    }
    nroots = 0;
    produced = 0;
    last_root = -1;
    follow = true;
    view_bottom = 0;
    for (int i = 0; i < nsel; i++) {
        root *r = &roots[nroots];
        memset(r, 0, sizeof(*r));
        r->pid = sel_pid[i];
        snprintf(r->name, sizeof(r->name), "%s", sel_name[i]);
        r->color = ROOT_COLORS[nroots % 8];
        r->fd = -1;
        if (!spawn_extrace(r)) {
            r->ended = true;
        }
        nroots++;
    }
    phase = PHASE_TRACE;
}

static void stop_trace(void) {
    for (int i = 0; i < nroots; i++) {
        root *r = &roots[i];
        if (r->fd >= 0) {
            close(r->fd);
            r->fd = -1;
        }
        if (r->child > 0) {
            kill(r->child, SIGTERM);
            waitpid(r->child, NULL, 0);
            r->child = 0;
        }
    }
    nroots = 0;
    produced = 0;
    phase = PHASE_SELECT;
    since_refresh = 1e9;   // resample procs on return
}

// drain whatever each extrace has emitted into the scrollback (non-blocking)
static void pump_traces(void) {
    for (int i = 0; i < nroots; i++) {
        root *r = &roots[i];
        if (r->fd < 0) {
            continue;
        }
        char buf[8192];
        long budget = 128 * 1024;       // cap per root per frame so a flood can't stall us
        ssize_t n;
        while (budget > 0 && (n = read(r->fd, buf, sizeof(buf))) > 0) {
            budget -= n;
            for (ssize_t k = 0; k < n; k++) {
                char c = buf[k];
                if (c == '\n') {
                    r->partial[r->plen] = 0;
                    push_line(i, r->partial);
                    r->plen = 0;
                } else if (c != '\r') {
                    if (r->plen >= LINE_MAX - 1) {
                        r->partial[r->plen] = 0;
                        push_line(i, r->partial);
                        r->plen = 0;
                    }
                    r->partial[r->plen++] = c;
                }
            }
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            if (r->plen > 0) {
                r->partial[r->plen] = 0;
                push_line(i, r->partial);
                r->plen = 0;
            }
            close(r->fd);
            r->fd = -1;
            r->ended = true;
            if (r->child > 0) {
                waitpid(r->child, NULL, WNOHANG);
            }
        }
    }
}

// ---------------------------------------------------------------- drawing
static void draw_titlebar(int w, const char *title, const char *right) {
    tui_fill(surface, 0, 0, w, 1, TUI_PANEL2);
    int px = tui_text(surface, 1, 0, "⟿ procsentry", ACCENT, TUI_PANEL2);
    tui_text_clip(surface, px + 2, 0, w - px - 3, title, TUI_DIM, TUI_PANEL2);
    if (right && *right) {
        int rw = tui_strwidth(right);
        tui_text(surface, w - rw - 1, 0, right, TUI_DIM, TUI_PANEL2);
    }
}

static void redraw_select(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_PLASMA, anim_t, BACK1, BACK2);

    char right[64];
    snprintf(right, sizeof(right), "%d selected · %d procs ", nsel, nprocs);
    draw_titlebar(w, hostname, right);

    int mx = w / 12;
    if (mx < 2) mx = 2;
    if (mx > 6) mx = 6;
    int x0 = mx, y0 = 2, pw = w - 2 * mx, ph = h - 4;
    if (pw < 30 || ph < 8) { x0 = 0; y0 = 1; pw = w; ph = h - 2; }
    tui_panel(surface, x0, y0, pw, ph, "Pick processes to trace", TUI_BORDER, TUI_WHITE, TUI_PANEL);

    int ix = x0 + 2, iw = pw - 4;
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "  %7s %-10s %5s  %s", "PID", "USER", "CPU%", "COMMAND");
    tui_text_clip(surface, ix, y0 + 1, iw, hdr, TUI_DIM, TUI_PANEL);
    tui_hline(surface, x0 + 1, y0 + 2, pw - 2, TUI_BORDER, TUI_PANEL);

    int ly0 = y0 + 3;
    int input_row = y0 + ph - 2;
    list_rows = input_row - ly0;
    if (list_rows < 1) list_rows = 1;
    pick_x0 = x0; pick_pw = pw; pick_ly0 = ly0;   // for mouse hit-testing

    if (hi < top) top = hi;
    if (hi >= top + list_rows) top = hi - list_rows + 1;
    if (top > nview - list_rows) top = nview - list_rows;
    if (top < 0) top = 0;

    if (nview == 0) {
        tui_text(surface, ix, ly0 + 1,
                 filter.len ? "No processes match the filter." : "No processes.",
                 TUI_DIM, TUI_PANEL);
    }
    for (int rr = 0; rr < list_rows; rr++) {
        int vi = top + rr;
        if (vi >= nview) {
            break;
        }
        const proc *p = &procs[view[vi]];
        int ry = ly0 + rr;
        bool issel = (vi == hi);
        bool checked = sel_index(p->pid) >= 0;
        int rbg = issel ? SEL_BG : TUI_PANEL;
        tui_fill(surface, x0 + 1, ry, pw - 2, 1, rbg);
        tui_text(surface, ix, ry, issel ? "▌" : " ", ACCENT, rbg);
        tui_text(surface, ix + 1, ry, checked ? "✓" : "·",
                 checked ? TUI_OK : TUI_FAINT, rbg);
        char row[320];
        snprintf(row, sizeof(row), "%7d %-10.10s %5.1f  %s",
                 p->pid, p->user, p->cpu, p->cmd);
        int txtfg = issel ? TUI_WHITE : (checked ? ACCENT : TUI_TEXT);
        tui_text_clip(surface, ix + 3, ry, iw - 3, row, txtfg, rbg);
    }

    if (top + list_rows < nview) {
        tui_text(surface, x0 + pw - 3, input_row - 1, "▾", TUI_DIM, TUI_PANEL);
    }
    if (top > 0) {
        tui_text(surface, x0 + pw - 3, ly0, "▴", TUI_DIM, TUI_PANEL);
    }

    // always-active search field inside the panel — just start typing to filter
    tui_fill(surface, x0 + 1, input_row, pw - 2, 1, SEL_BG);
    int lx = tui_text(surface, x0 + 1, input_row, " search ›", TUI_WHITE, SEL_BG);
    bool blink = ((int)(anim_t * 2)) & 1;
    char shown[320];
    if (filter.len) {
        snprintf(shown, sizeof(shown), " %s%s", filter.buf, blink ? "▏" : " ");
        tui_text_clip(surface, x0 + 1 + lx, input_row, pw - 3 - lx, shown, TUI_WHITE, SEL_BG);
    } else {
        snprintf(shown, sizeof(shown), " %s type a name to filter — e.g. sshd",
                 blink ? "▏" : " ");
        tui_text_clip(surface, x0 + 1 + lx, input_row, pw - 3 - lx, shown, TUI_DIM, SEL_BG);
    }

    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    tui_text_clip(surface, 1, h - 1, w - 2,
                  " type to search · ↑↓ move (hold = faster) · Space/click select · Enter trace · Esc clear/quit ",
                  TUI_DIM, TUI_PANEL2);

    tui_backdrop(TUI_BACK_PLASMA, anim_t, BACK1, BACK2);
}

static void redraw_trace(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_PLASMA, anim_t, BACK1, BACK2);

    int live = 0;
    long events = 0;
    for (int i = 0; i < nroots; i++) {
        live += !roots[i].ended;
        events += roots[i].count;
    }
    char title2[160], right[80];
    snprintf(title2, sizeof(title2), "tracing %d proc%s on %s",
             nroots, nroots == 1 ? "" : "s", hostname);
    snprintf(right, sizeof(right), "%ld exec%s · %d/%d live · %s ",
             events, events == 1 ? "" : "s", live, nroots,
             follow ? "following" : "paused");
    draw_titlebar(w, title2, right);

    // legend toolbar: a coloured chip per traced root
    tui_fill(surface, 0, 1, w, 1, TUI_PANEL2);
    int lx = 1;
    for (int i = 0; i < nroots && lx < w - 8; i++) {
        root *r = &roots[i];
        lx += tui_text(surface, lx, 1, "●", r->ended ? TUI_FAINT : r->color, TUI_PANEL2);
        char chip[80];
        snprintf(chip, sizeof(chip), " %d %s%s", r->pid, r->name, r->ended ? " ✕" : "");
        int cw = tui_strwidth(chip);
        if (cw > 22) cw = 22;
        tui_text_clip(surface, lx, 1, cw, chip, TUI_DIM, TUI_PANEL2);
        lx += cw + 2;
    }

    int mx = w / 12;
    if (mx < 2) mx = 2;
    if (mx > 6) mx = 6;
    int x0 = mx, y0 = 2, pw = w - 2 * mx, ph = h - 3;
    if (pw < 30 || ph < 6) { x0 = 0; y0 = 2; pw = w; ph = h - 3; }
    tui_panel(surface, x0, y0, pw, ph, "Exec trace", TUI_BORDER, ACCENT, TUI_PANEL);

    int ix = x0 + 2, iw = pw - 4;
    int rows = ph - 2;
    if (rows < 1) rows = 1;

    if (produced == 0) {
        int nd = ((int)(anim_t * 2)) % 4;
        char msg[120];
        snprintf(msg, sizeof(msg), "waiting for exec() events under the selected process%s%.*s",
                 nroots == 1 ? "" : "es", nd, "...");
        tui_text_clip(surface, ix, y0 + 1 + rows / 2, iw, msg, TUI_DIM, TUI_PANEL);
    }

    long hist = produced < SCROLL_MAX ? produced : SCROLL_MAX;
    long bottom = follow ? produced - 1 : view_bottom;
    long lowest = produced - hist;            // oldest line still in the ring
    if (bottom > produced - 1) bottom = produced - 1;
    if (bottom < lowest + rows - 1) bottom = lowest + rows - 1;
    long topln = bottom - rows + 1;

    for (int rr = 0; rr < rows; rr++) {
        long ln = topln + rr;
        int ry = y0 + 1 + rr;
        if (ln < lowest || ln < 0 || ln >= produced) {
            continue;
        }
        logline *L = &ring[ln % SCROLL_MAX];
        unsigned rc = (L->root < nroots) ? roots[L->root].color : (unsigned)TUI_TEXT;
        tui_text(surface, ix, ry, "▎", rc, TUI_PANEL);     // root-colour gutter
        if (L->kind == LL_HEADER) {                        // the selected parent
            int hx = tui_text(surface, ix + 2, ry, "▼ ", rc, TUI_PANEL);
            tui_text_clip(surface, ix + 2 + hx, ry, iw - 2 - hx, L->text, rc, TUI_PANEL);
        } else if (L->kind == LL_NOTICE) {                 // extrace diagnostic
            tui_text_clip(surface, ix + 4, ry, iw - 4, L->text, TUI_FAINT, TUI_PANEL);
        } else {                                           // an exec'd descendant
            int ind = L->depth * 2;
            if (ind > iw - 14) ind = iw - 14;
            if (ind < 0) ind = 0;
            int bx = ix + 4 + ind;
            tui_text(surface, bx, ry, "├ ", TUI_FAINT, TUI_PANEL);
            tui_text_clip(surface, bx + 2, ry, iw - 6 - ind, L->text, TUI_TEXT, TUI_PANEL);
        }
    }

    // scroll affordances
    if (!follow && bottom < produced - 1) {
        tui_text(surface, x0 + pw - 3, y0 + ph - 2, "▾", ACCENT, TUI_PANEL);
    }
    if (topln > lowest) {
        tui_text(surface, x0 + pw - 3, y0 + 1, "▴", TUI_DIM, TUI_PANEL);
    }

    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    char help[160];
    snprintf(help, sizeof(help),
             " ↑↓/wheel scroll · PgUp/PgDn · f %s · b back · q quit ",
             follow ? "paused→jump" : "follow");
    tui_text_clip(surface, 1, h - 1, w - 2, help, TUI_DIM, TUI_PANEL2);

    tui_backdrop(TUI_BACK_PLASMA, anim_t, BACK1, BACK2);
}

static void redraw(void) {
    if (phase == PHASE_SELECT) {
        redraw_select();
    } else {
        redraw_trace();
    }
}

// ---------------------------------------------------------------- scrolling
static void trace_scroll(long delta) {
    long hist = produced < SCROLL_MAX ? produced : SCROLL_MAX;
    long lowest = produced - hist;
    if (follow) {
        view_bottom = produced - 1;
        follow = false;
    }
    view_bottom += delta;
    if (view_bottom >= produced - 1) {
        view_bottom = produced - 1;
        follow = true;
    }
    if (view_bottom < lowest) {
        view_bottom = lowest;
    }
}

// ---------------------------------------------------------------- input
static void clamp_hi(void) {
    if (hi >= nview) hi = nview - 1;
    if (hi < 0) hi = 0;
}

// Accelerate held up/down keys: a single tap moves one row, but a sustained
// hold ramps the step up so a long process list scrolls quickly. Streak resets
// after a brief pause (anim_t stalls while input keeps arriving, so a fast hold
// builds the streak, while deliberate taps let it lapse).
static int nav_step(void) {
    if (anim_t - last_nav_t > 0.18) {
        nav_streak = 0;
    } else if (nav_streak < 24) {
        nav_streak++;
    }
    last_nav_t = anim_t;
    return 1 + nav_streak / 3;          // 1,1,1,2,2,2,... up to 9 rows/press
}

static void trace_char(char c) {
    int page = list_rows > 1 ? list_rows : 10;
    switch (c) {
        case 'q': quit_requested = true; break;
        case 'b': stop_trace(); break;
        case 'f': follow = true; view_bottom = produced - 1; break;
        case 'j': trace_scroll(1); break;
        case 'k': trace_scroll(-1); break;
        case 'g': trace_scroll(-(long)SCROLL_MAX); break;
        case 'G': follow = true; view_bottom = produced - 1; break;
        case ' ': trace_scroll(page); break;
        case 'd': trace_scroll(page); break;
        case 'u': trace_scroll(-page); break;
    }
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;

    if (event->type == TERMPAINT_EV_MOUSE) {
        if (event->mouse.action != TERMPAINT_MOUSE_PRESS) {
            return;                              // wheel + clicks arrive as PRESS
        }
        int btn = event->mouse.button;           // 4 = wheel up, 5 = wheel down
        if (phase == PHASE_SELECT) {
            if (btn == 4) { hi -= 3; clamp_hi(); }
            else if (btn == 5) { hi += 3; clamp_hi(); }
            else if (btn == 0) {                 // left click toggles the row's selection
                int row = event->mouse.y - pick_ly0;
                if (row >= 0 && row < list_rows &&
                    event->mouse.x >= pick_x0 && event->mouse.x < pick_x0 + pick_pw) {
                    int vi = top + row;
                    if (vi >= 0 && vi < nview) {
                        hi = vi;
                        toggle_sel(&procs[view[vi]]);
                    }
                }
            }
        } else {                                 // PHASE_TRACE: wheel scrolls the log
            if (btn == 4) { trace_scroll(-3); }
            else if (btn == 5) { trace_scroll(3); }
        }
        return;
    }

    if (event->type == TERMPAINT_EV_CHAR) {
        if (phase == PHASE_SELECT) {
            if (event->c.length == 1 && event->c.string[0] == ' ') {
                if (nview) toggle_sel(&procs[view[hi]]);   // Space selects, never the query
            } else {
                // type-to-search: any printable character filters the list live
                tui_input_char(&filter, event->c.string, event->c.length);
                rebuild_view();
            }
        } else if (event->c.length == 1) {
            trace_char(event->c.string[0]);
        }
        return;
    }
    if (event->type != TERMPAINT_EV_KEY) {
        return;
    }
    const char *a = event->key.atom;
    int page = list_rows > 1 ? list_rows : 10;

    if (phase == PHASE_SELECT) {
        if (a == termpaint_input_arrow_up()) { hi -= nav_step(); clamp_hi(); }
        else if (a == termpaint_input_arrow_down()) { hi += nav_step(); clamp_hi(); }
        else if (a == termpaint_input_page_up()) { hi -= page; clamp_hi(); }
        else if (a == termpaint_input_page_down()) { hi += page; clamp_hi(); }
        else if (a == termpaint_input_home()) { hi = 0; }
        else if (a == termpaint_input_end()) { hi = nview - 1; clamp_hi(); }
        else if (a == termpaint_input_space()) { if (nview) toggle_sel(&procs[view[hi]]); }
        else if (a == termpaint_input_backspace()) { tui_input_backspace(&filter); rebuild_view(); }
        else if (a == termpaint_input_enter()) { start_trace(); }
        else if (a == termpaint_input_escape()) {
            if (filter.len) { tui_input_reset(&filter); rebuild_view(); }
            else quit_requested = true;
        }
    } else {
        if (a == termpaint_input_arrow_up()) { trace_scroll(-1); }
        else if (a == termpaint_input_arrow_down()) { trace_scroll(1); }
        else if (a == termpaint_input_page_up()) { trace_scroll(-page); }
        else if (a == termpaint_input_page_down()) { trace_scroll(page); }
        else if (a == termpaint_input_home()) { trace_scroll(-(long)SCROLL_MAX); }
        else if (a == termpaint_input_end()) { follow = true; view_bottom = produced - 1; }
        else if (a == termpaint_input_escape()) { quit_requested = true; }
    }
}

int main(int argc, char **argv) {
    // Launch straight into a search: `procsentry sshd` (or PROCSENTRY_FILTER=sshd)
    // opens pre-filtered to sshd and its subtree.
    const char *initial = (argc > 1 && argv[1][0]) ? argv[1] : getenv("PROCSENTRY_FILTER");
#ifdef TUI_BUILD_GFX
    tui_gfx_detect("PROCSENTRY");
#endif
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint", event_callback, NULL, &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_cursor_visible(terminal, false);
    termpaint_terminal_set_mouse_mode(terminal, TERMPAINT_MOUSE_MODE_CLICKS);
#ifdef TUI_BUILD_GFX
    tui_gfx_start("PROCSENTRY");
#endif

    signal(SIGPIPE, SIG_IGN);
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = 0;
    tui_input_reset(&filter);
    if (initial && *initial) {
        tui_input_set(&filter, initial);     // first sample applies it
    }

    int frame_ms = 60;
    const char *fps = getenv("PROCSENTRY_FPS");
    if (fps && atoi(fps) > 0) {
        frame_ms = 1000 / atoi(fps);
    }

    int timeout = frame_ms;
    while (!quit_requested) {
        if (phase == PHASE_SELECT) {
            if (since_refresh >= refresh_interval) {
                sample_procs();
                rebuild_view();
                since_refresh = 0;
            }
        } else {
            pump_traces();
        }
        tui_frame_begin(surface);
        redraw();
        tui_frame_end(surface, terminal);
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            anim_t += frame_ms / 1000.0;
            since_refresh += frame_ms / 1000.0;
            timeout = frame_ms;
        }
    }

    if (phase == PHASE_TRACE) {
        stop_trace();
    }
    tui_gfx_stop();
    termpaint_terminal_free_with_restore(terminal);
    return 0;
}
