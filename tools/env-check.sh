#!/usr/bin/env bash
# Report availability of the host-side build toolchain used by MCubeAIOS.
set -u
missing=0
for t in gcc ld nasm make qemu-system-x86_64 xorriso mtools gdb python3 git objcopy; do
  if p="$(command -v "$t" 2>/dev/null)"; then
    printf '%-22s %s\n' "$t" "$p"
  else
    printf '%-22s MISSING\n' "$t"
    missing=$((missing+1))
  fi
done
echo "---"
printf 'cores: %s\n' "$(nproc)"
gcc --version 2>/dev/null | head -1
echo "missing=$missing"
