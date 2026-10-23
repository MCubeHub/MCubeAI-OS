set pagination off
set confirm off
set disassembly-flavor intel
target remote :1234
# Kernel is identity mapped at 2 MiB in M1; QEMU's gdbstub uses physical
# addresses, which is why breakpoints here are physical addresses.
hbreak *0x200000
layout src
continue
