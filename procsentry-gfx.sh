#!/bin/sh
# Build and run procsentry with the kitty-graphics backdrop (falls back to
# cells on terminals without graphics support). Extra args pass through, e.g.
#   ./procsentry-gfx.sh sshd
set -e
cd "$(dirname "$0")"
make build/procsentry-gfx
exec ./build/procsentry-gfx "$@"
