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
#   make iso        build a bootable UEFI ISO
#   make package    build ISO/IMG/VHD/QCOW2 and assemble dist/MCubeAIOS-<version>
#   make font       regenerate the 8x16 bitmap font
#   make clean

ARCH            ?= x86_64
BUILD           ?= build
VERSION         ?= dev

CC              := gcc
NASM            := nasm
MINGW           := x86_64-w64-mingw32-gcc

QEMU            ?= qemu-system-x86_64
QEMU_MACHINE    ?= pc
QEMU_MEM        ?= 512M
QEMU_CPUS       ?= 1
QEMU_ACCEL      ?= tcg
QEMU_NET        ?= -netdev user,id=net0 -device e1000,netdev=net0
RUN_TIMEOUT     ?= 90

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

# User-space programs. The init ELF is embedded into the kernel image; the
# coreutils are read from /BIN on the FAT32 ESP and launched by the shell.
USER_CFLAGS     := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
                   -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
                   -mno-sse -mno-sse2 -mno-mmx -fno-builtin -nostdlib -Iinclude -Iai
# The AI runtime does float math; the scheduler FXSAVE/FXRSTORes per task, so
# SSE2 is safe here. The rest of user space stays on x87 for compatibility.
AI_CFLAGS       := $(USER_CFLAGS) -msse -msse2
USER_LDFLAGS    := -nostdlib -static -Wl,-T,user/user.ld \
                   -Wl,--build-id=none -Wl,-z,max-page-size=4096 -Wl,-e,_start
USER_OBJ        := $(BUILD)/user/init.o
USER_ELF        := $(BUILD)/user/init.elf
USER_LGY        := $(BUILD)/user/init.lgy
LGYBUILD        := tools/lgybuild.py

USER_LIB_C_SRCS := $(shell find user/lib -name '*.c' | sort)
USER_LIB_S_SRCS := $(shell find user/lib -name '*.asm' | sort)
USER_LIB_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(USER_LIB_C_SRCS)) \
                   $(patsubst %.asm,$(BUILD)/%.o,$(USER_LIB_S_SRCS))

AI_TOOLS_C := $(shell find ai -name '*.c' 2>/dev/null | sort)
AI_TOOLS_O := $(patsubst %.c,$(BUILD)/%.o,$(AI_TOOLS_C))

USER_PROGS      := sh ls cat echo mkdir rm cp mv pwd sleep wc grep head tail \
                   uname true false clear env help lgyconv \
                   desktop terminal files editor calc image settings taskman \
                   sysinfo aiosd ai
USER_PROG_OBJS  := $(addprefix $(BUILD)/user/bin/,$(addsuffix .o,$(USER_PROGS)))
USER_PROG_ELFS  := $(addprefix $(BUILD)/user/bin/,$(addsuffix .elf,$(USER_PROGS)))
USER_PROG_LGYS  := $(addprefix $(BUILD)/user/bin/,$(addsuffix .lgy,$(USER_PROGS)))

USER_BLOB_C     := $(BUILD)/generated/user_init_blob.c
USER_BLOB_O     := $(BUILD)/generated/user_init_blob.o
KERNEL_OBJS     += $(USER_BLOB_O)

ESP_IMG         := $(BUILD)/esp.img
INITRD_IMG      := $(BUILD)/initrd.img
KERNEL_ELF      := $(BUILD)/kernel.elf
BOOT_EFI        := $(BUILD)/BOOTX64.EFI
SERIAL_LOG      := $(BUILD)/serial.log

DIST_NAME       := MCubeAIOS-$(VERSION)
PACKAGE_DIR     := dist/$(DIST_NAME)
ISO_IMG         := $(BUILD)/$(DIST_NAME).iso
DISK_IMG        := $(BUILD)/disk.img
VHD_IMG         := $(BUILD)/$(DIST_NAME).vhd
QCOW2_IMG       := $(BUILD)/$(DIST_NAME).qcow2
ISO_ROOT        := $(BUILD)/iso-root

.PHONY: all kernel image iso disk package run run-gui debug test test-disk \
        test-disk-gpt test-usb test-iso usb disk-gpt screenshot gui-test \
        clean font check-env

# Keep per-program objects between builds instead of treating them as
# intermediate files.
.SECONDARY:

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

$(USER_OBJ): user/init.c user/user.ld include/mcube/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  UCC     $<"

$(BUILD)/user/lib/%.o: user/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  UCC     $<"

$(BUILD)/user/lib/%.o: user/lib/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf -o $@ $<
	@echo "  UAS     $<"

$(BUILD)/user/bin/%.o: user/programs/%.c include/mcube/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  UCC     $<"

# The AI runtime is freestanding user-space code linked into aiosd only.
$(BUILD)/ai/%.o: ai/%.c ai/ai_runtime.h include/mcube/libc.h include/mcube/ai.h
	@mkdir -p $(dir $@)
	$(CC) $(AI_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  UCC     $<"

$(BUILD)/tiny.gguf: tools/gen_tiny_model.py
	@mkdir -p $(dir $@)
	python3 tools/gen_tiny_model.py $@
	@echo "  MODEL   $@"

# Live root filesystem: the loader reads this with firmware services only, so
# USB sticks and CD/DVD boots work without a kernel storage driver.
$(INITRD_IMG): $(KERNEL_ELF) $(USER_PROG_LGYS) $(BUILD)/tiny.gguf tools/mkinitrd.py
	@mkdir -p $(dir $@)
	python3 tools/mkinitrd.py $@ --kernel $(KERNEL_ELF) \
	    --model $(BUILD)/tiny.gguf \
	    $(foreach p,$(USER_PROGS),--prog $(BUILD)/user/bin/$(p).lgy)

$(USER_ELF): $(USER_OBJ) $(USER_LIB_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -o $@ $(USER_OBJ) $(USER_LIB_OBJS) $(LIBGCC)
	@echo "  ULD     $@"

$(BUILD)/user/bin/%.elf: $(BUILD)/user/bin/%.o $(USER_LIB_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -o $@ $< $(USER_LIB_OBJS) $(LIBGCC)
	@echo "  ULD     $@"

$(BUILD)/user/bin/aiosd.elf: $(BUILD)/user/bin/aiosd.o $(AI_TOOLS_O) \
                             $(USER_LIB_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -o $@ $(BUILD)/user/bin/aiosd.o $(AI_TOOLS_O) \
	      $(USER_LIB_OBJS) $(LIBGCC)
	@echo "  ULD     $@"

# ELF is only a link-time intermediate. Everything the kernel executes is
# packaged as LGY, which carries segments, arch and a payload CRC.
$(USER_LGY): $(USER_ELF) $(LGYBUILD)
	@mkdir -p $(dir $@)
	python3 $(LGYBUILD) $< $@

$(BUILD)/user/bin/%.lgy: $(BUILD)/user/bin/%.elf $(LGYBUILD)
	@mkdir -p $(dir $@)
	python3 $(LGYBUILD) $< $@

$(USER_BLOB_C): $(USER_LGY) tools/gen_blob.py
	@mkdir -p $(dir $@)
	python3 tools/gen_blob.py user_init_lgy $(USER_LGY) $@
	@echo "  BLOB    $@"

$(USER_BLOB_O): $(USER_BLOB_C) include/mcube/types.h
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<
	@echo "  CC      $<"

$(BOOT_EFI): boot/loader.c boot/efi.h include/mcube/bootinfo.h include/mcube/types.h
	@mkdir -p $(dir $@)
	$(MINGW) $(BOOT_CFLAGS) $(BOOT_LDFLAGS) -o $@ boot/loader.c
	@echo "  EFI     $@"

$(ESP_IMG): $(KERNEL_ELF) $(BOOT_EFI) $(USER_PROG_LGYS) $(BUILD)/tiny.gguf $(INITRD_IMG)
	@mkdir -p $(dir $@)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/BIN ::/MODELS
	mcopy -i $@ $(BOOT_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL_ELF) ::/kernel.elf
	# The FAT32 driver does not parse LFN entries yet, so the model must use
	# an 8.3 short name on the ESP.
	mcopy -i $@ $(BUILD)/tiny.gguf ::/MODELS/TINY.GGU
	mcopy -i $@ $(INITRD_IMG) ::/INITRD.IMG
	@for p in $(USER_PROGS); do \
	  mcopy -i $@ $(BUILD)/user/bin/$$p.lgy ::/BIN/`echo $$p | tr 'a-z' 'A-Z'`.LGY || exit 1; \
	done
	@echo "  IMG     $@"

# UEFI El Torito ISO. The FAT32 ESP image doubles as the boot image, and the
# GPT/basdat hybrid layout makes the ISO writable to a USB stick as well.
$(ISO_IMG): $(ESP_IMG) $(KERNEL_ELF) $(BOOT_EFI)
	@mkdir -p $(dir $@)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)
	cp $(ESP_IMG) $(ISO_ROOT)/ESP.IMG
	cp $(KERNEL_ELF) $(ISO_ROOT)/KERNEL.ELF
	cp $(BOOT_EFI) $(ISO_ROOT)/BOOTX64.EFI
	printf 'MCubeAIOS %s\n' '$(VERSION)' > $(ISO_ROOT)/VERSION.TXT
	xorriso -as mkisofs -R -J -V 'MCUBEAIOS' -A 'MCubeAIOS $(VERSION)' \
	    -e ESP.IMG -no-emul-boot -isohybrid-gpt-basdat \
	    -o $@ $(ISO_ROOT)
	@echo "  ISO     $@"

iso: $(ISO_IMG)

# Hybrid hard-disk / USB image: MBR + EFI system partition at LBA 2048.
# dd it to a USB stick (or use Rufus DD mode), or attach it as a disk.
$(DISK_IMG): $(ESP_IMG) tools/mkdisk.py
	@mkdir -p $(dir $@)
	python3 tools/mkdisk.py $@ $(ESP_IMG)
	@echo "  DISK    $@"

usb: $(DISK_IMG)

# Same layout with GPT (protective MBR + backup header). Useful on machines
# whose firmware prefers GPT disks and for users who repartition later.
DISK_GPT_IMG := $(BUILD)/disk-gpt.img

$(DISK_GPT_IMG): $(ESP_IMG) tools/mkdisk.py
	@mkdir -p $(dir $@)
	python3 tools/mkdisk.py --gpt $@ $(ESP_IMG)
	@echo "  DISK    $@"

disk-gpt: $(DISK_GPT_IMG)

test: $(ESP_IMG)
	@bash tools/test.sh

test-disk: $(DISK_IMG)
	@MEDIUM=disk bash tools/test.sh

test-disk-gpt: $(DISK_GPT_IMG)
	@MEDIUM=disk DISK_IMG=$(DISK_GPT_IMG) bash tools/test.sh

test-usb: $(DISK_IMG)
	@MEDIUM=usb bash tools/test.sh

test-iso: $(ISO_IMG)
	@MEDIUM=iso ISO_IMG=$(ISO_IMG) bash tools/test.sh

$(VHD_IMG): $(DISK_IMG)
	qemu-img convert -f raw -O vpc -o subformat=fixed $< $@
	@echo "  VHD     $@"

$(QCOW2_IMG): $(DISK_IMG)
	qemu-img convert -f raw -O qcow2 $< $@
	@echo "  QCOW2   $@"

disk: $(VHD_IMG) $(QCOW2_IMG) $(DISK_IMG)

package: $(ESP_IMG) $(KERNEL_ELF) $(BOOT_EFI) $(USER_PROG_LGYS) \
         $(BUILD)/tiny.gguf $(INITRD_IMG) $(ISO_IMG) $(VHD_IMG) $(QCOW2_IMG)
	rm -rf $(PACKAGE_DIR)
	mkdir -p $(PACKAGE_DIR)/bin
	cp $(ISO_IMG) $(PACKAGE_DIR)/
	cp $(ESP_IMG) $(PACKAGE_DIR)/$(DIST_NAME)-esp.img
	cp $(DISK_IMG) $(PACKAGE_DIR)/$(DIST_NAME)-disk.img
	cp $(DISK_IMG) $(PACKAGE_DIR)/$(DIST_NAME)-usb.img
	cp $(VHD_IMG) $(PACKAGE_DIR)/
	cp $(QCOW2_IMG) $(PACKAGE_DIR)/
	cp $(KERNEL_ELF) $(PACKAGE_DIR)/kernel.elf
	cp $(BOOT_EFI) $(PACKAGE_DIR)/BOOTX64.EFI
	cp $(BUILD)/tiny.gguf $(PACKAGE_DIR)/tiny.gguf
	cp $(INITRD_IMG) $(PACKAGE_DIR)/initrd.img
	cp $(USER_PROG_LGYS) $(PACKAGE_DIR)/bin/
	@printf '%s\n' \
	    'MCubeAIOS $(VERSION) (x86_64 UEFI)' \
	    '' \
	    'Contents:' \
	    '  $(DIST_NAME).iso        Bootable UEFI ISO (El Torito; dd-able to USB)' \
	    '  $(DIST_NAME)-disk.img   MBR disk image with EFI partition (hard disk)' \
	    '  $(DIST_NAME)-usb.img    Same image for USB sticks (dd / Rufus DD mode)' \
	    '  $(DIST_NAME)-esp.img    Raw FAT32 EFI system partition (64 MiB)' \
	    '  $(DIST_NAME).vhd        Disk image converted to VHD' \
	    '  $(DIST_NAME).qcow2      Disk image converted to QCOW2' \
	    '  initrd.img            Live root filesystem (loader reads it, no driver)' \
	    '  kernel.elf            Kernel ELF' \
	    '  BOOTX64.EFI           UEFI boot loader' \
	    '  bin/*.lgy             User-space LGY executables' \
	    '  tiny.gguf             Tiny AI model (also at /MODELS/TINY.GGU)' \
	    '' \
	    'Boot the ISO in QEMU (UEFI firmware required):' \
	    '  qemu-system-x86_64 -m 512M \' \
	    '    -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE_4M.fd \' \
	    '    -drive if=pflash,format=raw,file=OVMF_VARS_4M.fd \' \
	    '    -cdrom $(DIST_NAME).iso -serial stdio' \
	    '' \
	    'Boot the hard-disk image:' \
	    '  qemu-system-x86_64 -m 512M \' \
	    '    -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE_4M.fd \' \
	    '    -drive if=pflash,format=raw,file=OVMF_VARS_4M.fd \' \
	    '    -drive file=$(DIST_NAME)-disk.img,format=raw,if=ide -serial stdio' \
	    '' \
	    'Write the USB image (Linux):' \
	    '  sudo dd if=$(DIST_NAME)-usb.img of=/dev/sdX bs=4M status=progress' \
	    'Or use Rufus in DD mode; the ISO also boots from USB.' \
	    '' \
	    'Generated: %s' \
	    "$$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
	    > $(PACKAGE_DIR)/README.txt
	@printf 'version=%s\narch=x86_64\nbuilt_utc=%s\n' \
	    '$(VERSION)' "$$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
	    > $(PACKAGE_DIR)/VERSION.txt
	cd $(PACKAGE_DIR) && find . -type f ! -name SHA256SUMS.txt -print0 \
	    | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS.txt
	@echo "  PKG     $(PACKAGE_DIR)"

run: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine $(QEMU_MACHINE),accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial stdio -display none -no-reboot $(QEMU_NET)

run-gui: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine $(QEMU_MACHINE),accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial file:$(SERIAL_LOG) -display gtk,zoom-to-fit=on -no-reboot $(QEMU_NET)

debug: $(ESP_IMG)
	@cp -f $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) -machine $(QEMU_MACHINE),accel=$(QEMU_ACCEL) -cpu max -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(ESP_IMG),format=raw,if=ide \
	  -serial file:$(SERIAL_LOG) -display none -no-reboot $(QEMU_NET) -s -S

screenshot: $(ESP_IMG)
	@bash tools/screenshot.sh

gui-test: $(ESP_IMG)
	@bash tools/gui-test.sh

font:
	python3 tools/gen_font.py

clean:
	rm -rf $(BUILD)

check-env:
	@bash tools/env-check.sh

-include $(KERNEL_OBJS:.o=.d)
-include $(USER_OBJ:.o=.d) $(USER_LIB_OBJS:.o=.d) $(USER_PROG_OBJS:.o=.d)
-include $(AI_TOOLS_O:.o=.d)
