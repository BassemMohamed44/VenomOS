BITS 16
ORG 0x7C00

STAGE2_LOAD_SEGMENT equ 0x0000
STAGE2_LOAD_OFFSET  equ 0x8000
STAGE2_SECTOR_COUNT equ 6     
STAGE2_START_SECTOR equ 2      

start:
    cli                     
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    sti                    

    mov [BOOT_DRIVE], dl 

    mov si, msg_boot
    call print_string

    mov bx, STAGE2_LOAD_SEGMENT
    mov es, bx
    mov bx, STAGE2_LOAD_OFFSET
    mov al, STAGE2_SECTOR_COUNT
    mov cl, STAGE2_START_SECTOR
    mov dl, [BOOT_DRIVE]
    call disk_load

    mov si, msg_loaded
    call print_string

    mov dl, [BOOT_DRIVE]
    jmp STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET

    cli
    hlt

%include "print.asm"
%include "disk.asm"

BOOT_DRIVE db 0
msg_boot   db "VenomOS Stage1: Booting...", 13, 10, 0
msg_loaded db "VenomOS Stage1: Stage2 loaded. Jumping...", 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55
