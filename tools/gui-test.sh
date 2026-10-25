#!/usr/bin/env bash
# Drive the desktop through the QEMU monitor and capture screenshots.
#
# Boots headless, waits for USER_GUI_OK in the serial log, then:
#   * F1 opens the start menu               -> build/gui-menu.ppm
#   * Down, Enter launches the next app     -> build/gui-launch.ppm
#   * a PS/2 mouse click on the taskbar     -> build/gui-click.ppm
set -euo pipefail

BUILD=${BUILD:-build}
MON_PORT=${MON_PORT:-45455}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_MACHINE=${QEMU_MACHINE:-pc}
QEMU_MEM=${QEMU_MEM:-512M}
QEMU_CPUS=${QEMU_CPUS:-1}
QEMU_ACCEL=${QEMU_ACCEL:-tcg}
QEMU_NET=${QEMU_NET:--netdev user,id=net0 -device e1000,netdev=net0}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

cd "$(dirname "$0")/.."

if [ ! -f "$BUILD/esp.img" ]; then
    echo "gui-test: $BUILD/esp.img missing; run 'make image' first" >&2
    exit 1
fi

cp -f "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
rm -f "$BUILD"/gui-*.ppm "$BUILD/gui-test-serial.log"

"$QEMU" -machine "$QEMU_MACHINE,accel=$QEMU_ACCEL" -cpu max -m "$QEMU_MEM" \
    -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file="$BUILD/esp.img",format=raw,if=ide \
    -serial "file:$BUILD/gui-test-serial.log" -display none -no-reboot \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    $QEMU_NET &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$MON_PORT" "$BUILD" <<'PY'
import os, socket, sys, time

port = int(sys.argv[1])
build = sys.argv[2]
serial = os.path.join(build, "gui-test-serial.log")

sock = None
deadline = time.time() + 30
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1)
        break
    except OSError:
        time.sleep(0.25)
if sock is None:
    sys.exit("gui-test: monitor never came up")

sock.settimeout(3)
time.sleep(1.0)
try:
    sock.recv(65536)
except OSError:
    pass

def hmp(cmd, wait=0.6):
    sock.sendall((cmd + "\n").encode())
    time.sleep(wait)
    try:
        sock.recv(65536)
    except OSError:
        pass

# Wait for the desktop and all of its applications.
deadline = time.time() + 90
while time.time() < deadline:
    try:
        with open(serial, "r", errors="replace") as f:
            if "USER_GUI_OK" in f.read():
                break
    except OSError:
        pass
    time.sleep(0.5)
else:
    sys.exit("gui-test: USER_GUI_OK never appeared")

time.sleep(1.0)

# 1. Start menu via F1.
hmp("sendkey f1", 1.0)
hmp("screendump %s/gui-menu.ppm" % build, 1.0)

# 2. Move down one entry and launch it.
hmp("sendkey down", 0.4)
hmp("sendkey ret", 3.0)
hmp("screendump %s/gui-launch.ppm" % build, 1.0)

# 3. Relative PS/2 motion down to the bottom taskbar, then click a task button.
for _ in range(24):
    hmp("mouse_move -22 16", 0.05)
hmp("mouse_button 1", 0.2)
hmp("mouse_button 0", 0.4)
hmp("screendump %s/gui-click.ppm" % build, 1.0)

sock.close()
PY

echo "gui-test: screenshots in $BUILD/gui-*.ppm"
