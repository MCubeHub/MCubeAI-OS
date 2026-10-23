#!/usr/bin/env bash
# Capture the MCubeAIOS desktop to build/screen.ppm.
#
# QEMU is started headless with an HMP monitor on a loopback TCP port (unix
# sockets are not reliable on WSL drvfs mounts), then `screendump` is issued
# once the desktop has had time to start its applications.
set -euo pipefail

BUILD=${BUILD:-build}
SCREEN_DELAY=${SCREEN_DELAY:-18}
MON_PORT=${MON_PORT:-45454}
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
    echo "screenshot: $BUILD/esp.img missing; run 'make image' first" >&2
    exit 1
fi

cp -f "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
rm -f "$BUILD/screen.ppm" "$BUILD/screenshot-serial.log"

"$QEMU" -machine "$QEMU_MACHINE,accel=$QEMU_ACCEL" -cpu max -m "$QEMU_MEM" \
    -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file="$BUILD/esp.img",format=raw,if=ide \
    -serial "file:$BUILD/screenshot-serial.log" -display none -no-reboot \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    $QEMU_NET &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$MON_PORT" "$BUILD/screen.ppm" "$SCREEN_DELAY" <<'PY'
import socket, sys, time

port = int(sys.argv[1])
out = sys.argv[2]
delay = int(sys.argv[3])
deadline = time.time() + 15

# Wait for the monitor socket to accept connections.
sock = None
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1)
        break
    except OSError:
        time.sleep(0.25)
if sock is None:
    sys.exit("screenshot: monitor never came up")

sock.settimeout(2)
time.sleep(1.0)
try:
    sock.recv(65536)
except OSError:
    pass

# Give the desktop time to map the framebuffer and launch its windows.
time.sleep(max(1, delay))

sock.sendall(("screendump %s\n" % out).encode())
time.sleep(1.0)
try:
    print(sock.recv(65536).decode(errors="replace").strip())
except OSError:
    pass
sock.close()
PY

for _ in $(seq 1 20); do
    [ -s "$BUILD/screen.ppm" ] && break
    sleep 0.25
done

if [ -s "$BUILD/screen.ppm" ]; then
    echo "screenshot: wrote $BUILD/screen.ppm"
else
    echo "screenshot: capture failed" >&2
    exit 1
fi
