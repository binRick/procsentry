#!/usr/bin/env python3
# Drive a termpaint TUI app inside a PTY: send a scripted sequence of keys,
# discard the rendered output, and report the exit status. Used to smoke-test
# the menu-driven apps headlessly (cell mode — no real terminal responds to
# the graphics/auto-detection queries).
#
# Usage: drive_tui.py <prog> <key-script>
#   key-script is a ';'-separated list of tokens:
#     plain text  -> sent as-is
#     \r \n \t \e -> CR / LF / TAB / ESC
#     UP DOWN LEFT RIGHT -> arrow escape sequences
#     SP          -> a space
#     WAIT        -> pause ~0.4s with no key
import os, pty, time, sys, fcntl, termios, struct, select, signal

ESC = b"\x1b"
TOKENS = {
    "\\r": b"\r", "\\n": b"\n", "\\t": b"\t", "\\e": ESC,
    "UP": ESC + b"[A", "DOWN": ESC + b"[B",
    "RIGHT": ESC + b"[C", "LEFT": ESC + b"[D",
    "SP": b" ", "WAIT": b"",
}

def tok(t):
    return TOKENS.get(t, t.encode())

def main():
    prog = sys.argv[1]
    script = sys.argv[2] if len(sys.argv) > 2 else "WAIT;q"
    steps = script.split(";")

    mid, sid = pty.openpty()
    fcntl.ioctl(sid, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(sid, 0); os.dup2(sid, 1); os.dup2(sid, 2)
        os.close(mid)
        try:
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        except OSError:
            pass
        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")
        os.execvpe(prog, [prog], env)
        os._exit(127)
    os.close(sid)

    def drain(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([mid], [], [], 0.05)
            if r:
                try:
                    if not os.read(mid, 65536):
                        return
                except OSError:
                    return

    drain(float(os.environ.get("TUI_WARMUP", "3.0")))  # let auto-detection finish
    for s in steps:
        b = tok(s)
        if b:
            os.write(mid, b)
        drain(0.4)

    status = None
    for _ in range(40):
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
            break
        drain(0.1)
    if status is None:
        os.kill(pid, signal.SIGKILL)
        print("TIMEOUT: app did not exit")
        sys.exit(2)
    code = os.waitstatus_to_exitcode(status)
    print("exit code:", code)
    sys.exit(0 if code == 0 else 1)

main()
