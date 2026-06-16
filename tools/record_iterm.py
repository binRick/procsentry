#!/usr/bin/env python3
"""Record a demo running in iTerm2 to an animated GIF.

Opens an iTerm2 window, launches the demo in it, captures frames with
`screencapture` from a second iTerm2 window (TCC screen-recording
permission belongs to iTerm2, so the capture loop must run inside it),
then assembles the frames into a GIF with real frame timing.

Usage:
    python3 tools/record_iterm.py '<command>' <out.gif> \
        [--dur 6] [--warmup 2.5] [--width 800] [--key '1.0: '] ...

    --key 'T:KEYS' types KEYS into the demo T seconds after recording
    starts (a newline is appended by iTerm2; the demos ignore it).

Example:
    python3 tools/record_iterm.py \
        'FIREWORKS_MAXDIM=1024 ./build/fireworks-gfx' \
        docs/fireworks-kitty.gif --key '0.5: ' --key '1.2: '
"""

import argparse
import glob
import os
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REC_DIR = '/tmp/rec'
DEMO_BOUNDS = (60, 60, 1140, 760)     # x, y, x2, y2 — pt, not px
REC_BOUNDS = (1450, 850, 1800, 1000)  # small recorder window, out of the way

def osascript(script: str) -> str:
    out = subprocess.run(['osascript', '-e', script],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f"osascript failed: {out.stderr.strip()}")
    return out.stdout.strip()

def decode_key(s: str) -> str:
    # backslash escapes so --key can carry control bytes:
    #   \e ESC  \r Enter  \n LF  \t TAB  (\e[A \e[B \e[C \e[D = arrow keys)
    out, i = [], 0
    table = {'e': '\x1b', 'r': '\r', 'n': '\n', 't': '\t', 'b': '\x7f', '\\': '\\'}
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append(table.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)

def as_applescript(s: str) -> str:
    # build an AppleScript string expression, emitting control/non-ASCII bytes
    # as `(ASCII character N)` so ESC, CR etc. survive
    parts, run = [], ''
    for b in s.encode('utf-8'):
        if 32 <= b <= 126 and chr(b) not in '"\\':
            run += chr(b)
        else:
            if run:
                parts.append(f'"{run}"')
                run = ''
            parts.append(f'(ASCII character {b})')
    if run:
        parts.append(f'"{run}"')
    return ' & '.join(parts) if parts else '""'

def send_keys(win_id: int, text: str) -> None:
    # newline no: send EXACTLY these bytes (the modal TUI apps treat a stray
    # Enter as a real keystroke; send \r explicitly when you want one)
    osascript(f'tell application "iTerm2" to tell current session '
              f'of window id {win_id} to write text {as_applescript(text)} newline no')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command')
    ap.add_argument('out_gif')
    ap.add_argument('--dur', type=float, default=6.0)
    ap.add_argument('--warmup', type=float, default=2.5)
    ap.add_argument('--width', type=int, default=800)
    ap.add_argument('--fps', type=float, default=10.0)
    ap.add_argument('--crop-top', type=int, default=64,
                    help='pixels to crop off the top (window title + tab bar)')
    ap.add_argument('--skip', type=float, default=0.0,
                    help='seconds to drop from the start of the assembled GIF')
    ap.add_argument('--colors', type=int, default=256,
                    help='GIF palette size — fewer colors shrink dense scenes a lot')
    ap.add_argument('--key', action='append', default=[],
                    help="'T:KEYS' — type KEYS T seconds into the recording")
    args = ap.parse_args()

    keys = []
    for spec in args.key:
        t, _, k = spec.partition(':')
        keys.append((float(t), decode_key(k)))
    keys.sort()

    os.makedirs(REC_DIR, exist_ok=True)
    for f in glob.glob(f'{REC_DIR}/f_*.png') + [f'{REC_DIR}/stamps.txt', f'{REC_DIR}/DONE']:
        if os.path.exists(f):
            os.unlink(f)

    n_frames = int(args.dur * args.fps)
    gap = max(0.0, 1.0 / args.fps - 0.05)   # screencapture itself takes ~50ms

    # 1. demo window (brief pause between create and write: a write that
    #    races shell startup is silently lost)
    win_id = osascript(f'''
        tell application "iTerm2"
            set demoWin to (create window with default profile)
            set bounds of demoWin to {{{DEMO_BOUNDS[0]}, {DEMO_BOUNDS[1]}, {DEMO_BOUNDS[2]}, {DEMO_BOUNDS[3]}}}
            return id of demoWin
        end tell''')
    time.sleep(1.0)
    cmd = f"cd {ROOT} && clear && {args.command}"
    esc_cmd = cmd.replace('\\', '\\\\').replace('"', '\\"')
    osascript(f'tell application "iTerm2" to tell current session of window id {win_id} to write text "{esc_cmd}"')
    print(f"demo window id={win_id}")
    time.sleep(args.warmup)

    # 2. capture loop, run from a second iTerm2 window (inherits iTerm2's
    #    screen-recording permission; this shell doesn't have its own)
    with open('/tmp/rec_loop.sh', 'w') as f:
        f.write(f'''#!/bin/zsh
for i in $(seq -w 0 {n_frames - 1}); do
    screencapture -x -o -l {win_id} -t png {REC_DIR}/f_$i.png
    echo $EPOCHREALTIME >> {REC_DIR}/stamps.txt
    sleep {gap:.3f}
done
touch {REC_DIR}/DONE
''')
    rec_id = osascript(f'''
        tell application "iTerm2"
            set recWin to (create window with default profile)
            set bounds of recWin to {{{REC_BOUNDS[0]}, {REC_BOUNDS[1]}, {REC_BOUNDS[2]}, {REC_BOUNDS[3]}}}
            return id of recWin
        end tell''')
    time.sleep(1.0)
    for attempt in range(3):
        osascript(f'tell application "iTerm2" to tell current session of window id {rec_id} to write text "zsh /tmp/rec_loop.sh; exit"')
        for _ in range(10):
            time.sleep(0.5)
            if os.path.exists(f'{REC_DIR}/stamps.txt'):
                break
        if os.path.exists(f'{REC_DIR}/stamps.txt'):
            break
        print(f"recorder not started (attempt {attempt + 1}), retrying")

    # focus the demo window — iTerm2 throttles redraws of unfocused
    # windows, which collapses the recording into a slideshow
    osascript(f'''
        tell application "iTerm2"
            activate
            select window id {win_id}
        end tell''')

    # 3. scheduled keys while recording
    t0 = time.time()
    deadline = t0 + args.dur * 3 + 10
    for t, k in keys:
        wait = t0 + t - time.time()
        if wait > 0:
            time.sleep(wait)
        send_keys(win_id, k)
    while not os.path.exists(f'{REC_DIR}/DONE') and time.time() < deadline:
        time.sleep(0.3)

    # 4. quit the demo, close its window
    send_keys(win_id, "q")
    time.sleep(0.6)
    osascript(f'''
        tell application "iTerm2"
            try
                close window id {win_id}
            end try
        end tell''')

    # 5. assemble
    frames = sorted(glob.glob(f'{REC_DIR}/f_*.png'))
    frames = [f for f in frames if os.path.getsize(f) > 0]
    if len(frames) < 5:
        print(f"only {len(frames)} frames captured — aborting")
        sys.exit(1)
    stamps = [float(s) for s in open(f'{REC_DIR}/stamps.txt').read().split()]
    if args.skip > 0:
        n_skip = int(args.skip * args.fps)
        frames = frames[n_skip:]
        stamps = stamps[n_skip:]

    avg_s = ((stamps[-1] - stamps[0]) / (len(stamps) - 1)
             if len(stamps) > 1 else 1.0 / args.fps)
    imgs, durations = [], []
    for i, path in enumerate(frames):
        img = Image.open(path).convert('RGB')
        if args.crop_top:
            img = img.crop((0, args.crop_top, img.size[0], img.size[1]))
        w, h = img.size
        img = img.resize((args.width, int(h * args.width / w)), Image.LANCZOS)
        if args.colors < 256:
            img = img.quantize(colors=args.colors, method=Image.MEDIANCUT)
        imgs.append(img)
        if i + 1 < len(stamps):
            durations.append(int((stamps[i + 1] - stamps[i]) * 1000))
        else:
            durations.append(int(avg_s * 1000))

    out = os.path.join(ROOT, args.out_gif) if not os.path.isabs(args.out_gif) else args.out_gif
    imgs[0].save(out, save_all=True, append_images=imgs[1:],
                 duration=durations, loop=0, optimize=True)
    avg_ms = sum(durations) / len(durations)
    print(f"saved {out}: {len(imgs)} frames, ~{1000/avg_ms:.1f} fps, "
          f"{os.path.getsize(out) // 1024} KB")

if __name__ == '__main__':
    main()
