; MCubeAIOS BIOS stage2.
;
; Loaded by stage1 at 0000:7E00. Its job is to do everything the UEFI loader
; does for us there: enable A20, collect the E820 map, set a VBE linear
; framebuffer, stream the kernel and initrd out of the payload partition,
; build the identity/direct/kernel page tables, enter long mode, fill in the
; Boot Info Block and jump to the kernel entry point.
;
; Memory layout (all below 1 MiB unless stated otherwise):
;   0000:0400  BIOS data area -- must not be touched
;   0000:0500  payload header copy (512 bytes, left there by stage1)
;   0000:0700  DAP + scratch variables
;   0000:07E0  A20 test words
;   0000:0FF0  E820 entry count
;   0000:1000  E820 entries (24 bytes each)
;   0000:5000  Boot Info Block (412 bytes)
;   0000:6000  VBE controller info
;   0000:6200  VBE mode info
;   0000:7C00  stage1
;   0000:7E00  this file
;   0001:0000  64 KiB bounce buffer for disk reads (phys 0x10000)
;   0x0008F000  boot stack, top at 0x9F000 (above the page tables)
;   0x00040000  page tables (74 pages, up to 0x89FFF)
;   0x00200000  kernel
;   0x02000000  initrd

        bits 16
        org 0x7e00

%define HDR       0x0500
%define DAP       0x0700
%define PAYLBA    0x0710
%define DRIVE     0x0714
%define RETRY     0x0715
%define CURLBA    0x0718
%define REMSEC    0x071c
%define DEST      0x0720
%define BESTMODE  0x0724
%define LISTOFF   0x0726
%define LISTSEG   0x0728
%define BESTSCR   0x072a
%define TMODE     0x0730
%define MODECNT   0x0732          ; word: modes examined in the current pass
%define VBEANY    0x0734          ; byte: 1 = accept any resolution
%define VBE_MAXMODES 256          ; a sane mode list terminates well before this
%define DESTSEG   0x0740
%define RD_LBA    0x0744
%define RD_CNT    0x0748
%define E820N     0x0ff0
%define E820BUF   0x1000
%define BOOTINFO  0x5000
%define VBEINFO   0x6000
%define VBEMODE   0x6200
%define BOUNCE    0x10000
%define PTBASE    0x40000
%define STACKTOP  0x9f000

entry:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7c00
        sti
        cld
        mov [DRIVE], dl
        call serial_init
        mov si, msg_entry
        call puts

        call enable_a20
        jc a20_fail
        mov si, msg_a20
        call puts

        call do_e820
        mov si, msg_e820
        call puts

        call load_images
        mov si, msg_load
        call puts

        call check_long_mode
        jc .no64
        mov si, msg_vbe
        call puts
        call do_vbe

        jmp pm_entry

.no64:
        mov si, msg_no64
        call puts
.frozen:
        cli
        hlt
        jmp .frozen

a20_fail:
        mov si, msg_a20f
        call puts
.halt:
        cli
        hlt
        jmp .halt

; ---------------------------------------------------------------------------
; Long mode presence check (CPUID leaf 0x80000001, EDX bit 29). Entering long
; mode on a 32-bit-only CPU triple faults into a silent reset, so say so in
; plain text instead.
; ---------------------------------------------------------------------------
check_long_mode:
        pushad
        mov eax, 0x80000000
        cpuid
        cmp eax, 0x80000001
        jb .no
        mov eax, 0x80000001
        cpuid
        test edx, 0x20000000
        jz .no
        clc
        popad
        ret
.no:
        stc
        popad
        ret

; ---------------------------------------------------------------------------
; Console helpers: serial (0x3F8), port 0xE9 and BIOS teletype.
; ---------------------------------------------------------------------------
serial_init:
        push ax
        push dx
        mov dx, 0x3fb
        mov al, 0x80
        out dx, al
        mov dx, 0x3f8
        mov al, 1
        out dx, al
        mov dx, 0x3f9
        xor al, al
        out dx, al
        mov dx, 0x3fb
        mov al, 0x03
        out dx, al
        mov dx, 0x3fa
        mov al, 0xc7
        out dx, al
        mov dx, 0x3fc
        mov al, 0x0b
        out dx, al
        pop dx
        pop ax
        ret

putc:
        push ax
        push bx
        push dx
        mov bl, al
        mov ah, 0x0e
        mov bh, 0
        int 0x10
        mov dx, 0x3fd
.wait:
        in al, dx
        test al, 0x20
        jz .wait
        mov dx, 0x3f8
        mov al, bl
        out dx, al
        out 0xe9, al
        pop dx
        pop bx
        pop ax
        ret

puts:
        lodsb
        test al, al
        jz .done
        call putc
        jmp puts
.done:
        ret

; AX as four hexadecimal digits.
puthex:
        push ax
        push bx
        push cx
        mov cx, 4
.digit:
        rol ax, 4
        mov bx, ax
        and bx, 0x000f
        cmp bl, 10
        jb .num
        add bl, 'a' - 10
        jmp .emit
.num:
        add bl, '0'
.emit:
        mov al, bl
        call putc
        loop .digit
        pop cx
        pop bx
        pop ax
        ret

; AX as unsigned decimal (16-bit).
putdec:
        push ax
        push bx
        push cx
        push dx
        mov bx, 10
        xor cx, cx
.div:
        xor dx, dx
        div bx
        push dx
        inc cx
        test ax, ax
        jnz .div
.emit:
        pop dx
        mov al, dl
        add al, '0'
        call putc
        loop .emit
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; ---------------------------------------------------------------------------
; A20 gate.
; ---------------------------------------------------------------------------
a20_check:                              ; CF=1 -> A20 disabled
        push ds
        push es
        push si
        push di
        push ax
        xor ax, ax
        mov ds, ax
        mov si, 0x07e0                  ; free conventional memory, not the BDA
        mov ax, 0xffff
        mov es, ax
        mov edi, 0x07f0                 ; aliases 0x107E0 when A20 is on
        mov al, [ds:si]
        push ax
        mov al, [es:di]
        push ax
        mov byte [ds:si], 0x00
        mov byte [es:di], 0xff
        mov al, [ds:si]
        cmp al, 0xff
        je .off
        clc
        jmp .done
.off:
        stc
.done:
        pop ax
        mov [es:di], al
        pop ax
        mov [ds:si], al
        pop ax
        pop di
        pop si
        pop es
        pop ds
        ret

kbc_wait:
        push cx
        mov cx, 0xffff
.l:
        in al, 0x64
        test al, 0x02
        jz .done
        loop .l
.done:
        pop cx
        ret

kbc_wait_data:
        push cx
        mov cx, 0xffff
.l:
        in al, 0x64
        test al, 0x01
        jnz .done
        loop .l
.done:
        pop cx
        ret

enable_a20:
        call a20_check
        jnc .ok
        ; Fast A20 through port 0x92.
        in al, 0x92
        test al, 0x02
        jnz .kbc
        or al, 0x02
        and al, 0xfe
        out 0x92, al
        call a20_check
        jnc .ok
        ; Keyboard controller route.
.kbc:
        call kbc_wait
        mov al, 0xad
        out 0x64, al
        call kbc_wait
        mov al, 0xd0
        out 0x64, al
        call kbc_wait_data
        in al, 0x60
        push ax
        call kbc_wait
        mov al, 0xd1
        out 0x64, al
        call kbc_wait
        pop ax
        or al, 0x02
        out 0x60, al
        call kbc_wait
        mov al, 0xae
        out 0x64, al
        call kbc_wait
        call a20_check
        jnc .ok
        stc
        ret
.ok:
        clc
        ret

; ---------------------------------------------------------------------------
; E820 memory map, clamped to PMM_MAX_PHYS (64 GiB) and remapped to the
; boot_mem_type_t values the kernel understands.
; ---------------------------------------------------------------------------
do_e820:
        xor ebx, ebx
        mov di, E820BUF
        mov dword [E820N], 0
        mov bp, 600
.loop:
        push ds
        push es
        push di
        push bp
        mov eax, 0xe820
        mov edx, 0x534d4150              ; "SMAP"
        mov ecx, 24
        mov dword [di + 20], 1
        int 0x15
        pop bp
        pop di
        pop es
        pop ds
        jc .done
        cmp eax, 0x534d4150
        jne .done
        ; Drop anything entirely at or above 64 GiB.
        mov eax, [di + 4]
        cmp eax, 0x10
        jae .next
        ; Clamp length so base + len <= 64 GiB. esi:eax becomes the number of
        ; bytes left below the 64 GiB ceiling, as a 64-bit value.
        mov esi, 0x10
        sub esi, eax                    ; 0x10 - base_hi
        mov eax, [di + 0]
        neg eax                         ; -base_lo, CF set when base_lo != 0
        sbb esi, 0                      ; fold the borrow into the high half
        mov ecx, [di + 12]
        cmp ecx, esi
        ja .clamp
        jb .noclamp
        mov ecx, [di + 8]
        cmp ecx, eax
        jbe .noclamp
.clamp:
        mov [di + 12], esi
        mov [di + 8], eax
.noclamp:
        ; E820 type -> boot_mem_type.
        mov eax, [di + 16]
        cmp eax, 5
        jb .t_small
        je .t_bad
        mov eax, 2                      ; 6 and above: reserved
        jmp .t_set
.t_bad:
        mov eax, 6                      ; 5: bad memory
        jmp .t_set
.t_small:
        test eax, eax
        jnz .t_set                      ; 1..4 pass through
        mov eax, 2                      ; 0: unknown -> reserved
.t_set:
        mov [di + 16], eax
        mov dword [di + 20], 0          ; no ACPI 3.0 attribute bits
        add di, 24
        inc dword [E820N]
.next:
        dec bp
        jz .done
        test ebx, ebx
        jnz .loop
.done:
        ret

; ---------------------------------------------------------------------------
; Load the flattened kernel and (optionally) the initrd.
; The header LBA fields are already relative to the payload start, so the only
; adjustment is the payload's own absolute start LBA.
; ---------------------------------------------------------------------------
load_images:
        mov eax, [PAYLBA]
        add eax, [HDR + 8]              ; kernel stream LBA
        mov [CURLBA], eax
        mov eax, [HDR + 12]
        mov [REMSEC], eax
        mov eax, [HDR + 16]
        mov [DEST], eax
        call load_stream

        cmp dword [HDR + 0x24], 0
        je .done
        mov eax, [PAYLBA]
        add eax, [HDR + 0x20]           ; initrd stream LBA
        mov [CURLBA], eax
        mov eax, [HDR + 0x24]
        mov [REMSEC], eax
        mov eax, [HDR + 0x28]
        mov [DEST], eax
        call load_stream
.done:
        ret

; Stream [CURLBA]/[REMSEC] to [DEST] through the 64 KiB bounce buffer.
load_stream:
.next:
        cmp dword [REMSEC], 0
        je .done
        mov eax, [REMSEC]
        cmp eax, 120
        jbe .have
        mov eax, 120
.have:
        mov ebx, eax
        mov eax, [CURLBA]
        mov ecx, ebx
        mov word [DESTSEG], 0x1000      ; phys 0x10000
        call read_sectors
        jc .ioerr
        push ebx
        mov esi, BOUNCE
        mov edi, [DEST]
        mov ecx, ebx
        shl ecx, 9
        call unreal_copy
        pop ebx
        add [CURLBA], ebx
        sub [REMSEC], ebx
        mov eax, ebx
        shl eax, 9
        add [DEST], eax
        jmp .next
.ioerr:
        mov si, msg_io
        call puts
.halt:
        cli
        hlt
        jmp .halt
.done:
        ret

; ---------------------------------------------------------------------------
; unreal_copy: ESI = source, EDI = destination, ECX = bytes.
; Copies above 1 MiB by entering unreal mode and using 32-bit addressing.
; ---------------------------------------------------------------------------
unreal_copy:
        pushad
        call enter_unreal
        a32 rep movsb
        xor ax, ax
        mov ds, ax
        mov es, ax
        sti
        popad
        ret

; Load a 4 GiB data descriptor, drop back to real mode and leave the cached
; segment limits in place. Near jumps keep CS untouched.
enter_unreal:
        pushad
        cli
        lgdt [gdt_unreal_ptr]
        mov eax, cr0
        or al, 1
        mov cr0, eax
        jmp short .pm
.pm:
        mov ax, 0x08
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov eax, cr0
        and al, 0xfe
        mov cr0, eax
        jmp short .rm
.rm:
        popad
        ret

; ---------------------------------------------------------------------------
; Disk read: EAX = LBA, ECX = count, [DESTSEG] = destination segment.
; The cursor is kept in memory because INT 13h may clobber EAX/ECX.
; ---------------------------------------------------------------------------
read_sectors:
        mov [RD_LBA], eax
        mov [RD_CNT], cx
.block:
        mov byte  [DAP + 0], 16
        mov byte  [DAP + 1], 0
        mov word  [DAP + 2], 1
        mov word  [DAP + 4], 0
        mov ax, [DESTSEG]
        mov [DAP + 6], ax
        mov eax, [RD_LBA]
        mov [DAP + 8], eax
        mov dword [DAP + 12], 0
        mov byte [RETRY], 4
.try:
        mov si, DAP
        mov ah, 0x42
        mov dl, [DRIVE]
        int 0x13
        jnc .ok
        xor ah, ah
        mov dl, [DRIVE]
        int 0x13
        dec byte [RETRY]
        jnz .try
        stc
        ret
.ok:
        add word [DESTSEG], 32
        inc dword [RD_LBA]
        dec word [RD_CNT]
        jnz .block
        clc
        ret

; ---------------------------------------------------------------------------
; VBE: pick the largest 32 bpp direct-colour mode with a linear framebuffer
; and set it. The kernel framebuffer driver writes 32 bpp pixels only, so any
; other depth is useless.
;
; Progress is printed on screen because machines without a serial port have no
; other way to report where a boot stopped.
;
; VbeInfoBlock layout (both VBE 1.2 and 2.0+):
;   0x00 signature | 0x04 version | 0x06 oem string (far) | 0x0a capabilities
;   0x0e video mode list (far pointer: offset here, *segment at 0x10*)
;   0x12 total memory | 0x14 oem software revision
; Reading the segment from 0x12 picks up TotalMemory instead and walks a bogus
; mode list -- the reason this hung on old hardware.
; ---------------------------------------------------------------------------
do_vbe:
        push es
        push di
        push cx
        xor ax, ax
        mov es, ax
        mov di, VBEMODE
        mov cx, 128
        xor ax, ax
        rep stosw

        mov di, VBEINFO
        mov dword [VBEINFO], 0x32454256 ; request "VBE2"
        mov ax, 0x4f00
        int 0x10
        cmp ax, 0x004f
        jne .novbe
        cmp dword [VBEINFO], 0x41534556 ; "VESA"
        jne .novbe

        mov ax, [VBEINFO + 0x0e]
        mov [LISTOFF], ax
        mov ax, [VBEINFO + 0x10]        ; high half of VideoModePtr, not 0x12
        mov [LISTSEG], ax

        mov si, msg_vlist
        call puts
        mov ax, [LISTSEG]
        call puthex
        mov al, ':'
        call putc
        mov ax, [LISTOFF]
        call puthex
        mov si, msg_crlf
        call puts

        ; First pass: prefer 1024x768 or smaller so the UI stays legible on
        ; modest panels. Second pass: accept anything the BIOS offers.
        mov byte [VBEANY], 0
        call scan_modes
        cmp word [BESTMODE], 0xffff
        jne .have
        mov si, msg_vwide
        call puts
        mov byte [VBEANY], 1
        call scan_modes
        cmp word [BESTMODE], 0xffff
        je .nomode

.have:
        mov cx, [BESTMODE]
        call mode_info                  ; refresh the block for the winner
        mov si, msg_vmode
        call puts
        mov ax, [BESTMODE]
        call puthex
        mov si, msg_vat
        call puts
        mov ax, [VBEMODE + 0x12]
        call putdec
        mov al, 'x'
        call putc
        mov ax, [VBEMODE + 0x14]
        call putdec
        mov si, msg_crlf
        call puts

        xor ax, ax
        mov es, ax
        mov bx, [BESTMODE]
        or bx, 0x4000                   ; keep the linear framebuffer
        mov ax, 0x4f02
        int 0x10
        cmp ax, 0x004f
        jne .setfail
        mov cx, [BESTMODE]
        call mode_info                  ; VBE settles the LFB base on set
        jmp .out
.setfail:
        mov si, msg_vsetf
        call puts
        jmp .blank
.nomode:
        mov si, msg_vnone
        call puts
.blank:
        ; Nothing usable: leave a zeroed block so the kernel runs without a
        ; framebuffer instead of drawing into an unset mode.
        xor ax, ax
        mov es, ax
        mov di, VBEMODE
        mov cx, 128
        xor ax, ax
        rep stosw
        jmp .out
.novbe:
        mov si, msg_vnovbe
        call puts
        jmp .blank
.out:
        pop cx
        pop di
        pop es
        ret

; ---------------------------------------------------------------------------
; mode_info: CX = mode number, fills VBEMODE. Returns the INT 10h status in AX
; and restores everything else.
; ---------------------------------------------------------------------------
mode_info:
        push di
        push es
        xor dx, dx
        mov es, dx
        mov di, VBEMODE
        mov ax, 0x4f01
        int 0x10
        pop es
        pop di
        ret

; ---------------------------------------------------------------------------
; scan_modes: walk the mode list and remember the largest acceptable mode.
; [VBEANY] = 0 caps the resolution at 1024x768, non-zero accepts any size.
; The walk is bounded so a malformed list can never hang the boot.
; ---------------------------------------------------------------------------
scan_modes:
        mov dword [BESTSCR], 0
        mov word [BESTMODE], 0xffff
        mov word [MODECNT], 0
        mov ax, [LISTSEG]
        mov es, ax
        mov si, [LISTOFF]
.next:
        mov ax, [es:si]
        cmp ax, 0xffff
        je .done
        add si, 2
        inc word [MODECNT]
        cmp word [MODECNT], VBE_MAXMODES
        jae .done
        mov [TMODE], ax
        push es
        push si
        mov cx, [TMODE]
        call mode_info
        pop si
        pop es
        cmp ax, 0x004f
        jne .next
        mov ax, [VBEMODE + 0x00]
        and ax, 0x0091                  ; supported + graphics + linear framebuffer
        cmp ax, 0x0091
        jne .next
        cmp byte [VBEMODE + 0x19], 32
        jne .next
        cmp byte [VBEMODE + 0x1b], 6    ; direct colour
        jne .next
        cmp byte [VBEANY], 0
        jne .size
        ; Cap the preferred pass at 1920x1200. Anything wider is usually a
        ; synthetic mode the panel scales anyway, and it costs memory and fill
        ; rate for no visible gain. Second pass lifts the cap if none match.
        movzx eax, word [VBEMODE + 0x12]
        cmp eax, 1920
        ja .next
        movzx ecx, word [VBEMODE + 0x14]
        cmp ecx, 1200
        ja .next
.size:
        movzx eax, word [VBEMODE + 0x12]
        movzx ecx, word [VBEMODE + 0x14]
        imul eax, ecx
        cmp eax, [BESTSCR]
        jbe .next
        mov [BESTSCR], eax
        mov ax, [TMODE]
        mov [BESTMODE], ax
        jmp .next
.done:
        ret

; ---------------------------------------------------------------------------
; Protected mode, page tables, long mode.
; ---------------------------------------------------------------------------
pm_entry:
        cli
        lgdt [gdt_ptr]
        mov eax, cr0
        or eax, 1
        mov cr0, eax
        jmp dword 0x08:.pm32

        bits 32
.pm32:
        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov esp, STACKTOP

        ; Clear all 74 page-table pages (0x40000..0x89FFF).
        mov edi, PTBASE
        mov ecx, 74 * 4096 / 4
        xor eax, eax
        rep stosd

        ; PML4: identity (0), direct map (256), kernel window (511).
        mov dword [PTBASE + 0 * 8], 0x41000 | 3
        mov dword [PTBASE + 256 * 8], 0x42000 | 3
        mov dword [PTBASE + 511 * 8], 0x43000 | 3

        ; Identity PDPT: 4 entries covering the first 4 GiB.
        mov edi, 0x41000
        mov eax, 0x44000 | 3
        mov ecx, 4
.i1:
        mov [edi], eax
        add eax, 0x1000
        add edi, 8
        dec ecx
        jnz .i1

        ; Direct map PDPT: 64 entries covering 64 GiB. Entry i must point at the
        ; page directory that maps PA [i GiB, (i+1) GiB), i.e. the same linear
        ; PD array the identity map uses -- starting at 0x44000, not 0x48000.
        mov edi, 0x42000
        mov eax, 0x44000 | 3
        mov ecx, 64
.i2:
        mov [edi], eax
        add eax, 0x1000
        add edi, 8
        dec ecx
        jnz .i2

        ; Kernel window PDPT: entries 510 and 511 (last 2 GiB).
        mov dword [0x43000 + 510 * 8], 0x88000 | 3
        mov dword [0x43000 + 511 * 8], 0x89000 | 3

        ; Identity map (4 PDs) + direct map (64 PDs): 68 page directories
        ; filled linearly with 2 MiB pages starting at physical 0.
        mov edi, 0x44000
        mov ecx, 68 * 512
        xor eax, eax
        xor edx, edx
.pd:
        mov ebx, eax
        or ebx, 0x83                    ; present | rw | 2 MiB page
        mov [edi], ebx
        mov [edi + 4], edx
        add eax, 0x200000
        adc edx, 0
        add edi, 8
        dec ecx
        jnz .pd

        ; Kernel window: PML4[511] -> PDPT[510] and [511], each pointing at a
        ; page directory that maps 1 GiB starting at physical 0 (the kernel is
        ; linked at 0xFFFFFFFF80000000 + 0x200000, i.e. PA 0x200000).
        mov edi, 0x88000
        mov ecx, 2 * 512
        xor eax, eax
        xor edx, edx
.kpd:
        mov ebx, eax
        or ebx, 0x83
        mov [edi], ebx
        mov [edi + 4], edx
        add eax, 0x200000
        adc edx, 0
        add edi, 8
        dec ecx
        jnz .kpd

        mov eax, cr4
        or eax, 0x20                    ; PAE
        mov cr4, eax
        mov eax, PTBASE
        mov cr3, eax
        mov ecx, 0xc0000080
        rdmsr
        or eax, 0x100                   ; EFER.LME
        wrmsr
        mov eax, cr0
        or eax, 0x80000001              ; PG | PE
        mov cr0, eax
        jmp 0x18:.lm64

; ---------------------------------------------------------------------------
        bits 64
.lm64:
        mov ax, 0x20
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov rsp, STACKTOP
        cld

        ; Zero the Boot Info Block.
        mov rdi, BOOTINFO
        mov ecx, 412
        xor al, al
        rep stosb

        mov rax, 0x4d4355424f4f5431     ; "MCUBOOT1"
        mov [BOOTINFO + 0], rax
        mov dword [BOOTINFO + 8], 1
        mov dword [BOOTINFO + 12], 412

        ; Memory map.
        mov qword [BOOTINFO + 16], E820BUF
        mov eax, [E820N]
        mov [BOOTINFO + 24], eax
        mov dword [BOOTINFO + 28], 24

        ; Framebuffer.
        movzx eax, word [VBEMODE + 0x10]        ; BytesPerScanLine
        mov [BOOTINFO + 56], eax
        movzx eax, word [VBEMODE + 0x12]        ; XRes
        mov [BOOTINFO + 48], eax
        movzx ecx, word [VBEMODE + 0x14]        ; YRes
        mov [BOOTINFO + 52], ecx
        mov eax, [VBEMODE + 0x28]               ; PhysBasePtr
        mov [BOOTINFO + 32], rax
        movzx eax, word [VBEMODE + 0x10]
        imul rax, rcx
        mov [BOOTINFO + 40], rax                ; pitch * height
        xor eax, eax
        cmp byte [VBEMODE + 0x21], 16           ; red field at bit 16 -> BGRA
        jne .fmt
        mov eax, 1
.fmt:
        mov [BOOTINFO + 60], eax

        movzx ecx, byte [VBEMODE + 0x20]
        mov eax, 1
        shl rax, cl
        dec rax
        movzx ecx, byte [VBEMODE + 0x21]
        shl rax, cl
        mov [BOOTINFO + 64], eax

        movzx ecx, byte [VBEMODE + 0x22]
        mov eax, 1
        shl rax, cl
        dec rax
        movzx ecx, byte [VBEMODE + 0x23]
        shl rax, cl
        mov [BOOTINFO + 68], eax

        movzx ecx, byte [VBEMODE + 0x24]
        mov eax, 1
        shl rax, cl
        dec rax
        movzx ecx, byte [VBEMODE + 0x25]
        shl rax, cl
        mov [BOOTINFO + 72], eax

        ; Kernel and initrd.
        mov eax, [HDR + 16]
        mov [BOOTINFO + 92], rax
        mov eax, [HDR + 20]
        mov [BOOTINFO + 100], rax
        mov rax, [HDR + 24]
        mov [BOOTINFO + 108], rax
        mov eax, [HDR + 0x28]
        mov [BOOTINFO + 116], rax
        mov eax, [HDR + 0x2c]
        mov [BOOTINFO + 124], rax

        mov qword [BOOTINFO + 132], PTBASE
        mov qword [BOOTINFO + 140], STACKTOP
        mov dword [BOOTINFO + 148], 0
        mov dword [BOOTINFO + 152], 1

        lea rsi, [rel cmdline]
        mov rdi, BOOTINFO + 156
        mov ecx, cmdline_end - cmdline
        rep movsb

        mov rdi, BOOTINFO
        mov rax, [BOOTINFO + 108]
        jmp rax

; ---------------------------------------------------------------------------
; Descriptor tables.
; ---------------------------------------------------------------------------
        align 8
gdt:
        dq 0
        dq 0x00cf9a000000ffff           ; 0x08: 32-bit code
        dq 0x00cf92000000ffff           ; 0x10: 32-bit data
        dq 0x00af9a000000ffff           ; 0x18: 64-bit code
        dq 0x00af92000000ffff           ; 0x20: 64-bit data
gdt_end:
gdt_ptr:
        dw gdt_end - gdt - 1
        dd gdt

        align 8
gdt_unreal:
        dq 0
        dq 0x00cf92000000ffff           ; 0x08: 4 GiB flat data
gdt_unreal_end:
gdt_unreal_ptr:
        dw gdt_unreal_end - gdt_unreal - 1
        dd gdt_unreal

cmdline:
        db "console=ttyS0,115200 loglevel=debug", 0
cmdline_end:

msg_entry:
        db "MCUBOOT: BIOS stage2", 13, 10, 0
msg_a20:
        db "MCUBOOT: A20 on", 13, 10, 0
msg_a20f:
        db "MCUBOOT: A20 failed", 13, 10, 0
msg_e820:
        db "MCUBOOT: E820 done", 13, 10, 0
msg_load:
        db "MCUBOOT: kernel loaded", 13, 10, 0
msg_no64:
        db "MCUBOOT: CPU has no long mode (needs x86_64)", 13, 10, 0
msg_vbe:
        db "MCUBOOT: setting VBE mode", 13, 10, 0
msg_vlist:
        db "MCUBOOT: VBE modes at ", 0
msg_vwide:
        db "MCUBOOT: no 1024x768/32bpp mode, retry at any size", 13, 10, 0
msg_vnone:
        db "MCUBOOT: no 32bpp VBE mode", 13, 10, 0
msg_vmode:
        db "MCUBOOT: VBE mode ", 0
msg_vat:
        db " at ", 0
msg_vsetf:
        db "MCUBOOT: VBE mode set failed", 13, 10, 0
msg_vnovbe:
        db "MCUBOOT: VBE controller not available", 13, 10, 0
msg_crlf:
        db 13, 10, 0
msg_io:
        db "MCUBOOT: disk read failed", 13, 10, 0

        times 16384 - ($ - $$) db 0
%if ($ - $$) > 16384
        %error "stage2 exceeds the 16 KiB window (bounce buffer starts at 0x10000)"
%endif
