# procsentry

An interactive terminal tool: pick one or more processes, then watch a live
tree of every program their subtrees `exec()`. It's a TUI front-end for
[`extrace`](https://github.com/chneukirchen/extrace).

This file is the orientation for anyone (including Claude) continuing
development from this directory.

## What it is

Two panes:

1. **Picker** — a `ps --forest` process tree. Type to search (no mode to
   enter); the search is **subtree-aware**, so typing `sshd` shows sshd *and*
   every process running under it. Multi-select with Space/click, then Enter.
2. **Trace** — for each selected PID it spawns one `extrace -p PID` child and
   streams a live, colour-tagged tree of the `exec()` events under it. Each
   selected process is an explicit parent (`▼`) with its exec'd children nested
   under `├`.

The trace pane needs **Linux + root** (extrace uses the kernel proc connector,
`CAP_NET_ADMIN`). The picker works anywhere `ps` does (incl. macOS), which is
handy for developing the UI on a laptop.

## Layout

| File | What |
|---|---|
| `procsentry.c` | the whole program; renders termpaint **cells** |
| `procsentry-gfx.c` | one-line shim: `#define TUI_BUILD_GFX 1` then `#include "procsentry.c"` — turns on the kitty-graphics backdrop |
| `tui.c` / `tui.h` | small TUI toolkit: panels, lists, text input, colour theme, and the kitty backdrop engine (per-cell cut-out mask → the wallpaper shows only where no panel was drawn) |
| `kitty_gfx.c` / `kitty_gfx.h` | minimal [kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) support (RGBA framebuffer under the text layer) |
| `termpaint/` | **vendored** [termpaint](https://github.com/termpaint/termpaint) (BSL-1.0). No submodule — `make` just works |
| `Makefile` | builds `build/procsentry` and `build/procsentry-gfx`; `make install` |
| `procsentry.sh` / `procsentry-gfx.sh` | build-and-run launchers (pass args through) |
| `docs/` | README recordings (`procsentry-kitty.gif`, `procsentry-cells.gif`) |
| `tools/` | dev harness: `record_iterm.py` (GIFs), `drive_tui.py` (headless smoke test) |

**Origin:** procsentry was extracted from the `extracer` app in the `termfun`
repo. The `procsentry.c` / `procsentry-gfx.c` here are `extracer.c` /
`extracer-gfx.c` renamed (`extracer`→`procsentry`, `EXTRACER`→`PROCSENTRY`).
`tui.*` and `kitty_gfx.*` are shared verbatim with termfun — keep them in sync
if you change either copy.

## Build & run

```sh
make                      # build/procsentry and build/procsentry-gfx
make run                  # build + run the gfx build
sudo ./build/procsentry-gfx          # the real thing on Linux (root for the trace)
sudo ./build/procsentry-gfx sshd     # launch pre-filtered to sshd + its subtree
make install PREFIX=/usr DESTDIR=...  # for packaging
make clean
```

Toolchain: a C compiler + `make`. Only `-lm` is linked; termpaint is compiled
in from `termpaint/`. Builds warning-clean on clang (macOS) and gcc (Linux).

## Conventions

- **DRY pair**: `procsentry.c` is the entire program and renders cells;
  `procsentry-gfx.c` is the one-line shim that enables the kitty backdrop. Edit
  behaviour in `procsentry.c`; both binaries pick it up.
- **termpaint only** for terminal setup, input, and cell rendering — don't
  hand-roll escape sequences. Pixel effects go through `kitty_gfx.*`.
- **Env vars** (prefix `PROCSENTRY_`):
  - `PROCSENTRY_CELLS` — set to force pure cell rendering (skip kitty detection).
  - `PROCSENTRY_MAXDIM` — kitty framebuffer size cap (default 640).
  - `PROCSENTRY_FPS` — target frame rate.
  - `PROCSENTRY_BIN` — path to the `extrace` binary (default: found on `PATH`).
  - `PROCSENTRY_FILTER` — initial picker search (same as the positional arg).

## How the internals work

- **Picker sampling** (`sample_procs`): `ps -eo pid=,user=,pcpu=,args= --forest`
  on Linux (CPU-sorted flat list on macOS, which has no `--forest`). The
  `args` field keeps the `--forest` indentation verbatim; each proc stores its
  leading-space count as `indent` (tree depth).
- **Subtree search** (`rebuild_view` + `proc_matches`): in `ps --forest` DFS
  pre-order, a node's descendants are exactly the contiguous run of following
  procs with `indent` greater than the node's. So a match marks itself plus
  that run — pulling in the whole subtree even though children don't match the
  text. `cmd_body()` strips the `\_` tree art for selection labels/headers.
- **Type-to-search**: the picker has no modes. Printable chars (EV_CHAR) append
  to the live `filter`; Space (EV_KEY) toggles selection; Backspace edits the
  search; Enter traces; Esc clears the search then quits.
- **Nav acceleration** (`nav_step`): a single ↑/↓ tap moves one row; a sustained
  hold ramps to ~9 rows/press (`anim_t` stalls while input keeps arriving, so a
  fast hold builds the streak and deliberate taps let it lapse).
- **Mouse**: `TERMPAINT_MOUSE_MODE_CLICKS`. Wheel (button 4/5) scrolls both
  panes; left-click (button 0) toggles the row under the cursor (picker
  geometry is stashed in `pick_x0/pick_pw/pick_ly0` each redraw for hit-testing).
- **Trace** (`start_trace`/`spawn_extrace`/`pump_traces`): one `fork`+`execvp`
  of `extrace -p PID` per selected pid, stdout+stderr captured on a non-blocking
  pipe. Each frame drains the pipes (bounded per root), assembles lines, and
  pushes them into a fixed ring buffer. A line is rendered as a header (`▼`, the
  selected parent), an exec event (nested under `├`, depth from extrace's own
  indentation), or a dimmed `extrace:` diagnostic. Re-emits the parent header
  when the active root switches (like `tail -f` reprinting a filename).
  `stop_trace` SIGTERMs and reaps the children.
- **Backdrop**: in `procsentry-gfx`, `tui.c` fills a framebuffer everywhere the
  app did *not* draw a panel (a per-cell cut-out mask), so the UI floats over an
  animated plasma wallpaper. Cell mode paints a quiet dark gradient instead.

## Testing / dev

- **Headless smoke test** (no real terminal): `tools/drive_tui.py` runs a binary
  in a PTY and feeds a `;`-separated key script (`UP`/`DOWN`/`\r`/`SP`/`WAIT`),
  reporting exit status. Set `TUI_WARMUP=5` and `PROCSENTRY_CELLS=1`.
- **Faithful screenshots / driving with pyte**: launch the binary in a PTY,
  feed keys, and render `pyte.Screen` to text. The first keystroke after
  termpaint's ~2s auto-detection is swallowed in a PTY — send a sacrificial key
  first, and use a 5s+ warmup. `pip install pyte` (`--break-system-packages` on
  Homebrew Python).
- The trace pane only does anything on **Linux as root**. The dev/test Linux
  host used during development is `mia` (CentOS Stream 10, has `extrace` at
  `/usr/local/bin/extrace`, reachable over ssh). Spin up recognisable workloads
  with `systemd-run --unit=zza --quiet bash -c 'while :; do : ZZALPHA; ls / >/dev/null; sleep 0.4; done'`.

## README recordings (`tools/record_iterm.py`)

GIFs are **real iTerm2 sessions**, captured on macOS, of the binary running over
ssh on a Linux host (so the trace pane actually works):

```sh
python3 tools/record_iterm.py \
  "ssh -t mia 'cd procsentry && PROCSENTRY_MAXDIM=768 ./build/procsentry-gfx'" \
  docs/procsentry-kitty.gif --dur 15 --warmup 6 --skip 1 --fps 10 --colors 88 \
  --key '1.2:z' --key '4.5:sshd' --key ...
```

Quirks (handled by the tool, don't regress): the capture loop must run *inside*
iTerm2 (TCC screen-recording permission belongs to iTerm2); iTerm2 throttles
unfocused windows, so it refocuses the demo window — which means **the recording
needs the iTerm2 window frontmost and visible for the whole capture**; a
backgrounded window yields 0 frames (`screencapture` can't grab it). For
`procsentry`, drive the demo with `--key`: a sacrificial first key, then type a
search term (e.g. `sshd`), then Space/Down to select, then `\r` to trace.

## Releases

- Tagged `vX.Y.Z`, built and published with `gh release create`.
- Linux binary + **RHEL9 RPM** are built in a Rocky 9 container (the dev host is
  el10, so its native binary won't run on el9 — build el9 artifacts in a
  `rockylinux:9` container with `gcc make rpm-build`). The RPM spec installs
  `procsentry` and `procsentry-gfx` to `/usr/bin` via `make install`.
