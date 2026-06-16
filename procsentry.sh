#!/bin/sh
# Build and run procsentry (cell rendering). termpaint is vendored, so this
# just builds and runs — no downloads. Extra args are passed through, e.g.
#   ./procsentry.sh sshd
set -e
cd "$(dirname "$0")"
make build/procsentry
exec ./build/procsentry "$@"
