#!/bin/sh
# Sidecar startup: create the virtual UART pair, then run the C reader.
# Both processes share this container's /dev/pts namespace so the symlinks
# /tmp/ttyS1 and /tmp/ttyS2 resolve correctly.

set -e

# Clean any stale links from a previous run (shared /tmp volume).
rm -f /tmp/ttyS1 /tmp/ttyS2 /tmp/qr.sock

echo "[start.sh] launching socat virtual UART pair"
socat -d -d \
    pty,raw,echo=0,link=/tmp/ttyS1,mode=666 \
    pty,raw,echo=0,link=/tmp/ttyS2,mode=666 &
SOCAT_PID=$!

# Wait until both symlinks exist (up to 5 s).
for i in 1 2 3 4 5; do
    [ -e /tmp/ttyS1 ] && [ -e /tmp/ttyS2 ] && break
    sleep 1
done

if [ ! -e /tmp/ttyS1 ] || [ ! -e /tmp/ttyS2 ]; then
    echo "[start.sh] FATAL: socat did not create PTY links" >&2
    exit 1
fi

echo "[start.sh] socat ready (pid=$SOCAT_PID); starting qr-c"
exec ./qr-c
