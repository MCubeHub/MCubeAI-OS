#!/usr/bin/env bash
# Boot the image headless, wait for the last acceptance marker, then assert
# every milestone marker. Exits as soon as USER_GUI_OK is seen so `make test`
# does not have to sit through the full RUN_TIMEOUT on a healthy boot.
set -uo pipefail

BUILD=${BUILD:-build}
MEDIUM=${MEDIUM:-esp}
ISO_IMG=${ISO_IMG:-$BUILD/MCubeAIOS-dev.iso}
RUN_TIMEOUT=${RUN_TIMEOUT:-90}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_MACHINE=${QEMU_MACHINE:-pc}
QEMU_MEM=${QEMU_MEM:-512M}
QEMU_CPUS=${QEMU_CPUS:-1}
QEMU_ACCEL=${QEMU_ACCEL:-tcg}
QEMU_NET=${QEMU_NET:--netdev user,id=net0 -device e1000,netdev=net0}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
SERIAL_LOG=${SERIAL_LOG:-$BUILD/serial-$MEDIUM.log}
AHCI_IMG=${AHCI_IMG:-$BUILD/ahci.img}
DISK_IMG=${DISK_IMG:-$BUILD/disk.img}

case "$MEDIUM" in
    esp)  BOOT_MEDIA=(-drive file="$BUILD/esp.img",format=raw,if=ide) ;;
    disk) BOOT_MEDIA=(-drive file="$DISK_IMG",format=raw,if=ide) ;;
    usb)  BOOT_MEDIA=(-device qemu-xhci \
                     -drive file="$DISK_IMG",format=raw,if=none,id=usbstick \
                     -device usb-storage,drive=usbstick) ;;
    iso)  BOOT_MEDIA=(-cdrom "$ISO_IMG") ;;
    *)    echo "unknown MEDIUM=$MEDIUM (esp|disk|usb|iso)" >&2; exit 2 ;;
esac

cd "$(dirname "$0")/.."

cp -f "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
rm -f "$SERIAL_LOG"

# Standalone SATA disk for the AHCI driver: the marker at LBA 0 proves that
# READ DMA EXT returned real data, not just a successful IDENTIFY.
if [ ! -f "$AHCI_IMG" ]; then
    dd if=/dev/zero of="$AHCI_IMG" bs=1M count=8 status=none
    printf 'MCUBE-AHCI-TEST' | dd of="$AHCI_IMG" bs=1 conv=notrunc status=none
fi

"$QEMU" -machine "$QEMU_MACHINE,accel=$QEMU_ACCEL" -cpu max -m "$QEMU_MEM" \
    -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    "${BOOT_MEDIA[@]}" \
    -device ich9-ahci,id=ahci \
    -drive file="$AHCI_IMG",format=raw,if=none,id=ahcidisk \
    -device ide-hd,drive=ahcidisk,bus=ahci.0 \
    -serial "file:$SERIAL_LOG" -display none -no-reboot \
    $QEMU_NET &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

deadline=$((SECONDS + RUN_TIMEOUT))
while (( SECONDS < deadline )); do
    if grep -q "USER_GUI_OK" "$SERIAL_LOG" 2>/dev/null; then
        sleep 0.5
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done

cleanup
trap - EXIT

echo "--- serial log ---"
cat "$SERIAL_LOG"

markers=(
    MCUBE_BOOT_OK SCHED_TEST_OK VFS_TEST_OK
    NET_PING_OK NET_DNS_OK NET_TEST_OK
    USER_INIT_OK USER_EFAULT_OK USER_EINVAL_OK
    USER_VFS_OK USER_DIR_OK USER_STAT_OK USER_TTY_OK USER_NET_OK
    USER_SHELL_OK USER_REDIR_OK
    AI_DAEMON_OK AI_MODEL_OK AI_TOOL_OK AI_CHAT_OK
    HWINFO_OK AHCI_TEST_OK
    USER_DESKTOP_OK USER_GUI_OK
)

missing=()
for m in "${markers[@]}"; do
    grep -q "$m" "$SERIAL_LOG" || missing+=("$m")
done

if [ ${#missing[@]} -ne 0 ]; then
    echo "TEST FAIL: missing markers: ${missing[*]}" >&2
    exit 1
fi

echo "TEST PASS: boot + scheduler + VFS + net + user/syscall + AI + GUI markers found"
