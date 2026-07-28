; ============================================================
; stage1.asm
;
; This is the Master Boot Record (MBR). The BIOS loads this
; exact 512-byte sector from the first sector of the boot disk
; to physical address 0x7C00 and jumps to it in 16-bit real
; mode, with DL holding the boot drive number.
;
; Responsibilities of Stage 1:
;   1. Set up a known-good segment/stack environment
;   2. Print a boot message to prove we are alive
;   3. Read Stage 2 off disk using BIOS int 13h
;   4. Jump control to Stage 2 (passing the boot drive along in DL)
;
; Stage 1 must fit in 512 bytes and end with the boot
; signature 0xAA55, or the BIOS will refuse to boot it.
; ============================================================

BITS 16
ORG 0x7C00

; ------------------------------------------------------------
; Where Stage 2 will be loaded in memory.
; 0x0000:0x8000 is comfortably above stage1 (0x7C00-0x7DFF)
; and below the 0x9FC00 conventional memory ceiling.
; ------------------------------------------------------------
STAGE2_LOAD_SEGMENT equ 0x0000
STAGE2_LOAD_OFFSET  equ 0x8000
STAGE2_SECTOR_COUNT equ 6      ; load 6 sectors (3072 bytes) for stage2
STAGE2_START_SECTOR equ 2      ; stage2 sits immediately after the boot sector

start:
    cli                     ; no interrupts while we set up segments/stack

    ; ------------------------------------------------------------
    ; Real mode has no concept of "flat" addresses; every address
    ; is segment:offset. We zero every segment register so that
    ; all our ORG 0x7C00 offsets resolve correctly, and we place
    ; the stack safely below our own code (growing down from 0x7C00).
    ; ------------------------------------------------------------
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    sti                     ; safe to re-enable interrupts now

    mov [BOOT_DRIVE], dl    ; BIOS passes boot drive number in DL - save it

    mov si, msg_boot
    call print_string

    ; ------------------------------------------------------------
    ; Load Stage 2 from disk into ES:BX
    ; ------------------------------------------------------------
    mov bx, STAGE2_LOAD_SEGMENT
    mov es, bx
    mov bx, STAGE2_LOAD_OFFSET
    mov al, STAGE2_SECTOR_COUNT
    mov cl, STAGE2_START_SECTOR
    mov dl, [BOOT_DRIVE]
    call disk_load

    mov si, msg_loaded
    call print_string

    ; ------------------------------------------------------------
    ; Hand off control to Stage 2, passing the boot drive number
    ; forward in DL - Stage 2 will need it to load the kernel.
    ; ------------------------------------------------------------
    mov dl, [BOOT_DRIVE]
    jmp STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET

; Should never return here, but just in case:
    cli
    hlt

; ------------------------------------------------------------
; Shared routines
; ------------------------------------------------------------
%include "print.asm"
%include "disk.asm"

; ------------------------------------------------------------
; Data
; ------------------------------------------------------------
BOOT_DRIVE db 0
msg_boot   db "VenomOS Stage1: Booting...", 13, 10, 0
msg_loaded db "VenomOS Stage1: Stage2 loaded. Jumping...", 13, 10, 0

; ------------------------------------------------------------
; Boot sector padding and signature
; The BIOS requires the last two bytes of sector 1 to be 0x55AA
; (0xAA55 little-endian) or it will not recognize this as bootable.
; ------------------------------------------------------------
times 510-($-$$) db 0
dw 0xAA55
