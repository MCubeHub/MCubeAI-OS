#!/usr/bin/env bash
# Boot the built ESP image in QEMU, wait for a serial marker, optionally take
# a framebuffer screenshot, then shut the VM down.
#
#   tools/run-qemu.sh [seconds] [screenshot.ppm]
#
# Requires: qemu-system-x86_64, OVMF, python3 (Pillow optional for PNG).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
TIMEOUT="${1:-20}"
SHOT="${2:-}"
MARKER="${MARKER:-MCUBE_BOOT_OK}"

[[ -f "${BUILD}/esp.img" ]] || { echo "build/esp.img missing; run make" >&2; exit 1; }
cp -f "${OVMF_VARS}" "${BUILD}/OVMF_VARS.fd"
rm -f "${BUILD}/serial.log" "${BUILD}/monitor.sock"

EXTRA=()
MON_PORT="${MON_PORT:-4555}"
if [[ -n "${SHOT}" ]]; then
  EXTRA=(-monitor "tcp:127.0.0.1:${MON_PORT},server,nowait")
fi

qemu-system-x86_64 \
  -machine "${QEMU_MACHINE:-pc}",accel="${QEMU_ACCEL:-tcg}" -cpu max -m 512M -smp 1 \
  -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
  -drive if=pflash,format=raw,file="${BUILD}/OVMF_VARS.fd" \
  -drive file="${BUILD}/esp.img",format=raw,if=ide \
  -serial "file:${BUILD}/serial.log" -display none -no-reboot -net none \
  "${EXTRA[@]}" &
QPID=$!
trap 'kill ${QPID} 2>/dev/null || true; wait ${QPID} 2>/dev/null || true' EXIT

ok=0
for _ in $(seq 1 $((TIMEOUT * 4))); do
  if grep -q "${MARKER}" "${BUILD}/serial.log" 2>/dev/null; then ok=1; break; fi
  if ! kill -0 ${QPID} 2>/dev/null; then break; fi
  sleep 0.25
done

if [[ -n "${SHOT}" && ${ok} -eq 1 ]]; then
  sleep 0.4
  python3 - "${MON_PORT}" "${SHOT}" <<'PY' || true
import socket, sys, time
port, out = int(sys.argv[1]), sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", port))
time.sleep(0.2)
s.sendall(("screendump %s\n" % out).encode())
time.sleep(0.4)
s.close()
PY
  if [[ -f "${SHOT}" ]]; then
    python3 - "${SHOT}" <<'PY' || true
import sys
try:
    from PIL import Image
    src = sys.argv[1]
    Image.open(src).save(src[:-4] + ".png")
    print("png: " + src[:-4] + ".png")
except Exception as e:
    print("ppm kept: %s" % e)
PY
  fi
fi

echo "--- serial log ---"
cat "${BUILD}/serial.log" 2>/dev/null || true
echo "------------------"
if [[ ${ok} -eq 1 ]]; then
  echo "PASS: ${MARKER}"
  exit 0
fi
echo "FAIL: '${MARKER}' not seen within ${TIMEOUT}s"
exit 1
