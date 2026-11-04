#!/usr/bin/env bash
# Temporary visual-regression helper: drive the pointer to absolute positions
# through the QEMU monitor and screendump each one.
#   SHOTS="name:x:y,name:x:y,..." bash tools/gui-verify.sh
set -euo pipefail

BUILD=${BUILD:-build}
MON_PORT=${MON_PORT:-45456}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_MACHINE=${QEMU_MACHINE:-pc}
QEMU_MEM=${QEMU_MEM:-512M}
QEMU_CPUS=${QEMU_CPUS:-1}
QEMU_ACCEL=${QEMU_ACCEL:-tcg}
QEMU_NET=${QEMU_NET:--netdev user,id=net0 -device e1000,netdev=net0}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
SHOTS=${SHOTS:-base:900:120}

cd "$(dirname "$0")/.."

cp -f "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
rm -f "$BUILD"/vf-*.ppm "$BUILD/vf-serial.log"

"$QEMU" -machine "$QEMU_MACHINE,accel=$QEMU_ACCEL" -cpu max -m "$QEMU_MEM" \
    -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file="$BUILD/esp.img",format=raw,if=ide \
    -serial "file:$BUILD/vf-serial.log" -display none -no-reboot \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    $QEMU_NET &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$MON_PORT" "$BUILD" "$SHOTS" "$BUILD/vf-serial.log" <<'PY'
import os, socket, sys, time

port = int(sys.argv[1]); build = sys.argv[2]
shots = sys.argv[3]; serial = sys.argv[4]

sock = None
deadline = time.time() + 30
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1); break
    except OSError:
        time.sleep(0.25)
if sock is None:
    sys.exit("gui-verify: monitor never came up")
sock.settimeout(3); time.sleep(1.0)
try: sock.recv(65536)
except OSError: pass

def hmp(cmd, wait=0.35):
    sock.sendall((cmd + "\n").encode()); time.sleep(wait)
    try: sock.recv(65536)
    except OSError: pass

deadline = time.time() + int(os.environ.get("GUI_WAIT", "120"))
while time.time() < deadline:
    try:
        with open(serial, "r", errors="replace") as f:
            if "USER_GUI_OK" in f.read(): break
    except OSError: pass
    time.sleep(0.5)
else:
    # Set WAIT_GUI=0 while debugging a boot that never reports readiness.
    if os.environ.get("WAIT_GUI", "1") == "1":
        sys.exit("gui-verify: USER_GUI_OK never appeared")
    print("gui-verify: USER_GUI_OK missing, continuing anyway")
time.sleep(1.5)

def step(dx, dy):
    # PS/2 packets carry signed bytes, so never push more than 100 at a time.
    while dx or dy:
        sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
        hmp("mouse_move %d %d" % (sx, sy), 0.06)
        dx -= sx; dy -= sy

lx = ly = 0

def move_abs(x, y):
    # Slam into the top-left corner first so the position is known, then step out.
    for _ in range(20):
        hmp("mouse_move -100 -100", 0.05)
    time.sleep(0.4)
    step(x, y)

for spec in shots.split(","):
    parts = spec.split(":")
    name, xs, ys = parts[0], parts[1], parts[2]
    flags = parts[3] if len(parts) > 3 else ""
    x, y = int(xs), int(ys)

    if "m" in flags:
        step(x - lx, y - ly)          # relative, for drags
    else:
        move_abs(x, y)
    lx, ly = x, y
    hmp("mouse_move 0 0", 0.25)

    if "z" in flags:
        # Long zig-zag drag: enough painted frames for the paint statistics.
        for k in range(360):
            hmp("mouse_move %d 0" % (4 if (k // 3) % 2 == 0 else -4), 0.006)

    if "p" in flags:
        hmp("mouse_button 1", 0.4)
    if "r" in flags:
        hmp("mouse_button 0", 0.4)
    if "c" in flags:
        hmp("mouse_button 1", 0.3)
        hmp("mouse_button 0", 0.8)

    hmp("screendump %s/vf-%s.ppm" % (build, name), 0.8)
    print("shot %s at %d,%d %s" % (name, x, y, flags))

sock.close()
PY

echo "gui-verify: screenshots in $BUILD/vf-*.ppm"
