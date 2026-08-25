#!/usr/bin/env bash
# Headless boot helper: boots the ISO in QEMU, captures the serial log, takes a
# framebuffer screendump, then shuts the machine down.
#
#   tools/boot.sh [seconds] [PROGRAM]
#
# Artifacts:
#   build/serial.log   the kernel serial log
#   build/screen.ppm   the framebuffer contents at the end of the run
set -u

SECS=${1:-25}
PROGRAM=${2:-explorer.exe}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

LOG=build/serial.log
SHOT=build/screen.ppm
MON=$(mktemp -u /tmp/ntos-mon.XXXXXX)
# The QEMU monitor splits arguments on whitespace, so the screendump target has
# to be a path without spaces; the project root contains one.
RAWSHOT=$(mktemp -u /tmp/ntos-shot.XXXXXX).ppm

make -s PROGRAM="$PROGRAM" iso build/disk.img >/dev/null || exit 1
rm -f "$LOG" "$SHOT"

qemu-system-x86_64 -m 1024M -no-reboot -no-shutdown -boot d \
    -cdrom build/ntos.iso \
    -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
    -serial "file:$LOG" -display none \
    -monitor "unix:$MON,server,nowait" &
QEMU=$!

for _ in $(seq 1 50); do [ -S "$MON" ] && break; sleep 0.1; done
sleep "$SECS"

if [ -S "$MON" ]; then
    { printf 'screendump %s\n' "$RAWSHOT"; sleep 2; } | \
        socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
    [ -f "$RAWSHOT" ] && cp "$RAWSHOT" "$SHOT"
fi

kill "$QEMU" 2>/dev/null
wait "$QEMU" 2>/dev/null
rm -f "$MON" "$RAWSHOT"

echo "serial: $(wc -l < "$LOG" 2>/dev/null || echo 0) lines -> $LOG"
[ -f "$SHOT" ] && echo "screen: $(head -c 32 "$SHOT" | tr '\n' ' ') -> $SHOT"
