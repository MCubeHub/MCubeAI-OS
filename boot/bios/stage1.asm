; MCubeAIOS BIOS stage1 (MBR).
;
; Fits in the 432 bytes available before the partition table. It scans the
; four primary partition entries for the one whose first sector starts with
; the "MCBB" payload magic, then loads stage2 (described by that header) to
; 0000:7E00 and jumps to it.
;
; The UEFI path is unaffected: this sector only runs when the firmware boots
; us in legacy/CSM mode.

        bits 16
        org 0x7c00

; Scratch lives at 0x0700..0x074F, i.e. free conventional memory. Do not use
; 0x0400..0x04FF: that is the BIOS data area (COM port bases, equipment word,
; video state). Some firmware reads it back inside INT 13h.
%define HDR     0x0500          ; payload header copy (512 bytes)
%define DAP     0x0700          ; disk address packet
%define PAYLBA  0x0710          ; payload start LBA
%define DRIVE   0x0714          ; boot drive number
%define RETRY   0x0715          ; read retry counter
%define DESTSEG 0x0748          ; current read destination segment
%define LBA     0x0740          ; read cursor (dword)
%define RCOUNT  0x0744          ; sectors left (word)
%define PARTS   0x7dbe          ; 0x7c00 + 446: first partition entry
%define STAGE2  0x7e00

_start:
        jmp 0x0000:start        ; normalise CS to base 0 whatever the BIOS did
start:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7c00
        sti
        cld
        mov [DRIVE], dl

        ; The sector is loaded at 0x7C00 with DS=0, so the partition table is at
        ; absolute 0x7DBE. `org 0x7c00` makes the assembler resolve labels to
        ; absolute addresses, but a bare literal like "446" would point into the
        ; BIOS data area instead.
        mov si, PARTS           ; absolute 0x7DBE, i.e. the MBR partition table
        mov cx, 4
.scan:
        cmp byte [si + 4], 0    ; unused slot?
        je .next
        cmp dword [si + 8], 0   ; no start LBA?
        je .next
        push cx
        push si
        mov eax, [si + 8]
        mov [PAYLBA], eax
        mov word [DESTSEG], HDR >> 4
        call read_sector
        jc .pop_next
        cmp dword [HDR], 0x4242434d     ; "MCBB"
        je .found
.pop_next:
        pop si
        pop cx
.next:
        add si, 16
        loop .scan
        jmp .fail

.found:
        pop si
        pop cx
        movzx ecx, word [HDR + 4]       ; stage2 size in sectors
        test ecx, ecx
        jz .fail
        mov eax, [PAYLBA]
        inc eax                         ; stage2 starts right after the header
        mov word [DESTSEG], STAGE2 >> 4
        call read_sectors
        jc .fail
        mov dl, [DRIVE]
        jmp 0x0000:STAGE2

.fail:
        mov si, msg_fail
        call puts
.halt:
        cli
        hlt
        jmp .halt

; ---------------------------------------------------------------------------
; puts: DS:SI -> NUL terminated string, teletype output.
; ---------------------------------------------------------------------------
puts:
        lodsb
        test al, al
        jz .done
        mov ah, 0x0e
        mov bx, 0x0007
        int 0x10
        jmp puts
.done:
        ret

; ---------------------------------------------------------------------------
; read_sector / read_sectors
;   in:  EAX = first LBA, ECX = sector count, [DESTSEG] = destination segment
;   out: CF set on failure
;
; The cursor lives in memory because INT 13h is free to clobber EAX/ECX.
; ---------------------------------------------------------------------------
read_sector:
        mov ecx, 1
read_sectors:
        mov [LBA], eax
        mov [RCOUNT], cx
.block:
        mov byte  [DAP + 0], 16
        mov byte  [DAP + 1], 0
        mov word  [DAP + 2], 1
        mov word  [DAP + 4], 0
        mov ax, [DESTSEG]
        mov [DAP + 6], ax
        mov eax, [LBA]
        mov [DAP + 8], eax
        mov dword [DAP + 12], 0
        mov byte [RETRY], 4
.try:
        mov si, DAP
        mov ah, 0x42
        mov dl, [DRIVE]
        int 0x13
        jnc .ok
        xor ah, ah                      ; reset the drive, then retry
        mov dl, [DRIVE]
        int 0x13
        dec byte [RETRY]
        jnz .try
        stc
        ret
.ok:
        add word [DESTSEG], 32          ; 512 bytes = 0x20 paragraphs
        inc dword [LBA]
        dec word [RCOUNT]
        jnz .block
        clc
        ret

msg_fail:
        db "MCubeAIOS: no BIOS boot payload", 13, 10, 0

        times 432 - ($ - $$) db 0
%if ($ - $$) > 432
        %error "stage1 exceeds the 432 byte MBR limit"
%endif
