# MCubeAIOS build system (x86_64, UEFI + freestanding ELF kernel).
#
# Host requirements (Debian/Ubuntu):
#   build-essential nasm qemu-system-x86 xorriso mtools gdb \
#   gcc-mingw-w64-x86-64 ovmf
#
# Targets:
#   make            build the FAT EFI system partition image
#   make run        boot it in QEMU (headless, serial on stdout)
#   make run-gui    boot with a graphical window (needs a display/WSLg)
#   make test       boot and assert the kernel reached MCUBE_BOOT_OK
#   make font       regenerate the 8x16 bitmap font
#   make clean

ARCH            ?= x86_64
BUILD           ?= build

CC              := gcc
NASM            := nasm
MINGW           := x86_64-w64-mingw32-gcc

QEMU            ?= qemu-system-x86_64
QEMU_MEM        ?= 512M
QEMU_CPUS       ?= 1
QEMU_ACCEL      ?= tcg
RUN_TIMEOUT     ?= 20

OVMF_CODE       ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS       ?= /usr/share/OVMF/OVMF_VARS_4M.fd

KERNEL_CFLAGS   := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
                   -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                   -fno-omit-frame-pointer -mno-red-zone \
                   -mno-sse -mno-sse2 -mno-mmx \
                   -mcmodel=kernel -Iinclude
KERNEL_LDFLAGS  := -nostdlib -static -Wl,-T,kernel/linker.ld \
                   -Wl,--build-id=none -Wl,-z,max-page-size=4096
LIBGCC          := $(shell $(CC) -print-libgcc-file-name)

BOOT_CFLAGS     := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
                   -ffreestanding -fno-stack-protector -mno-red-zone \
                   -mno-stack-arg-probe -fno-builtin -Iinclude -Iboot
BOOT_LDFLAGS    := -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
                   -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
                   -Wl,--no-insert-timestamp -Wl,--file-alignment,0x1000 \
                   -Wl,--section-alignment,0x1000

KERNEL_C_SRCS   := $(shell find kernel -name '*.c' | sort)
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' | sort)
KERNEL_OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C_SRCS)) \
                   $(patsubst %.asm,$(BUILD)/%.o,$(KERNEL_ASM_SRCS))

# User-space programs. The init ELF is embedded into the kernel image for
# M4; once the VFS lands it will be read from the FAT32 ESP instead.
USER_CFLAGS     := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
                   -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
                   -mno-sse -mno-sse2 -mno-mmx -fno-builtin -nostdlib -Iinclude
USER_LDFLAGS    := -nostdlib -static -Wl,-T,user/user.ld \
                   -Wl,--build-id=none -Wl,-z,max-page-size=4096 -Wl,-e,_start
USER_OBJ        := $(BUILD)/user/init.o
USER_ELF        := $(BUILD)/user/init.elf
USER_BLOB_C     := $(BUILD)/generated/user_init_blob.c
USER_BLOB_O     := $(BUILD)/generated/user_init_blob.o
KERNEL_OBJS     += $(USER_BLOB_O)

ESP_IMG         := $(BUILD)/esp.img
KERNEL_ELF      := $(BUILD)/kernel.elf
BOOT_EFI        := $(BUILD)/BOOTX64.EFI
SERIAL_LOG      := $(BUILD)/serial.log

.PHONY: all kernel image run run-gui debug test clean font check-env

all: image

kernel: $(KERNEL_ELF)

image: $(ESP_IMG)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS) $(LIBGCC)
	@echo "  LD      $@"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  CC      $<"

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf -o $@ $<
	@echo "  AS      $<"

$(USER_OBJ): user/init.c user/user.ld include/mcube/syscall.h include/mcube/types.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<
	@echo "  UCC     $<"

$(USER_ELF): $(USER_OBJ) user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -o $@ $(USER_OBJ) $(LIBGCC)
	@echo "  ULD     $@"

$(USER_BLOB_C): $(USER_ELF) tools/gen_blob.py
	@mkdir -p $(dir $@)
	python3 tools/gen_blob.py user_init_elf $(USER_ELF) $@
	@echo "  BLOB    $@"

$(USER_BLOB_O): $(USER_BLOB_C) include/mcube/types.h
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  CC      $<"

$(BOOT_EFI): boot/loader.c boot/efi.h include/mcube/bootinfo.h include/mcube/types.h
	@mkdir -p $(dir $@)
	$(MINGW) $(BOOT_CFLAGS) $(BOOT_LDFLAGS) -o $@ boot/loader.c
	@echo "  EFI     $@"

$(ESP_IMG): $(KERNEL_ELF) $(BOOT_EFI)
	@mkdir -p $(dir $@)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $(BOOT_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL_ELF) ::/kernel.elf
	@echo "  IMG     $@"

run: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine q35,accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial stdio -display none -no-reboot -net none

run-gui: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine q35,accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial file:$(SERIAL_LOG) -display gtk -no-reboot -net none

debug: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine q35,accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial file:$(SERIAL_LOG) -display none -no-reboot -net none -s -S

test: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	@rm -f $(SERIAL_LOG)
	@timeout $(RUN_TIMEOUT) $(QEMU) -machine q35,accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial file:$(SERIAL_LOG) -display none -no-reboot -net none || true
	@echo "--- serial log ---"
	@cat $(SERIAL_LOG)
	@grep -q "MCUBE_BOOT_OK" $(SERIAL_LOG) && grep -q "SCHED_TEST_OK" $(SERIAL_LOG) \
	  && grep -q "USER_INIT_OK" $(SERIAL_LOG) \
	  && grep -q "USER_EFAULT_OK" $(SERIAL_LOG) \
	  && grep -q "USER_EINVAL_OK" $(SERIAL_LOG) \
	  && echo "TEST PASS: boot + scheduler + user/syscall markers found" \
	  || (echo "TEST FAIL: boot/scheduler/user marker not found" && exit 1)

font:
	python3 tools/gen_font.py

clean:
	rm -rf $(BUILD)

check-env:
	@bash tools/env-check.sh

-include $(KERNEL_OBJS:.o=.d)
