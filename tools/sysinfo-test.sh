#!/usr/bin/env bash
# Verify the hardware inventory end to end:
#   1. type `sysinfo` on the serial console (CLI output)
#   2. launch System Info from the Start menu and capture the window
#
# Usage: tools/sysinfo-test.sh   (BUILD=build3 to pick another image)
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

cd "$(dirname "$0")/.."

if [ ! -f "$BUILD/esp.img" ]; then
    echo "sysinfo-test: $BUILD/esp.img missing; run 'make image' first" >&2
    exit 1
fi

cp -f "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
rm -f "$BUILD"/sysinfo-*.ppm "$BUILD/sysinfo-serial.log"

"$QEMU" -machine "$QEMU_MACHINE,accel=$QEMU_ACCEL" -cpu max -m "$QEMU_MEM" \
    -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file="$BUILD/esp.img",format=raw,if=ide \
    -serial "file:$BUILD/sysinfo-serial.log" -display none -no-reboot \
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
serial = os.path.join(build, "sysinfo-serial.log")

sock = None
deadline = time.time() + 30
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1)
        break
    except OSError:
        time.sleep(0.25)
if sock is None:
    sys.exit("sysinfo-test: monitor never came up")

sock.settimeout(3)
time.sleep(1.0)
try:
    sock.recv(65536)
except OSError:
    pass

def hmp(cmd, wait=0.4):
    sock.sendall((cmd + "\n").encode())
    time.sleep(wait)
    try:
        sock.recv(65536)
    except OSError:
        pass

def log():
    try:
        with open(serial, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""

# Wait for the interactive shell.
deadline = time.time() + 120
while time.time() < deadline:
    if "mcube$" in log():
        break
    time.sleep(0.5)
else:
    sys.exit("sysinfo-test: shell prompt never appeared")

time.sleep(0.5)
for ch in "sysinfo":
    hmp("sendkey " + ch, 0.08)
hmp("sendkey ret", 2.0)

deadline = time.time() + 20
while time.time() < deadline:
    if "PCI (" in log():
        break
    time.sleep(0.5)

# Start menu -> End -> Enter selects the last entry (System Info).
hmp("sendkey f1", 0.8)
hmp("sendkey end", 0.4)
hmp("sendkey ret", 4.0)
hmp("screendump %s/sysinfo-gui.ppm" % build, 1.0)
sock.close()
PY

echo "sysinfo-test: serial in $BUILD/sysinfo-serial.log, screen in $BUILD/sysinfo-gui.ppm"
