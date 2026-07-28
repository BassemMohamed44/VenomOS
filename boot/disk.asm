; ============================================================
; disk.asm
; BIOS disk read routine (CHS addressing, int 13h/ah=02h)
;
; Generalized in Phase 3 to accept an arbitrary starting sector,
; since we now load two things off disk (Stage 2, then the
; kernel), starting at different sectors.
; ============================================================

; disk_load
; Loads sectors from disk starting at a caller-specified CHS
; sector (cylinder 0, head 0 always - fine for the small floppy
; image sizes this project uses).
;
; Input : AL = number of sectors to read
;         CL = starting sector number (1-based; sector 1 is the boot sector)
;         DL = drive number (0x00 = first floppy, as passed in by BIOS at boot)
;         ES:BX = destination buffer
; Output: on success, returns with carry clear
; On failure: prints a diagnostic and halts - never returns garbage
disk_load:
    mov [REQUESTED_SECTORS], al   ; remember what we asked for, to verify after

    mov ah, 0x02        ; BIOS function: read sectors into memory
    mov ch, 0x00        ; cylinder 0
    mov dh, 0x00        ; head 0
    int 0x13

    jc disk_error       ; carry flag set -> BIOS reported an error

    cmp al, [REQUESTED_SECTORS]  ; did we get as many sectors as requested?
    jne sectors_error
    ret

disk_error:
    mov si, disk_error_msg
    call print_string
    mov dl, ah          ; BIOS error code is returned in AH
    call print_hex
    jmp halt_system

sectors_error:
    mov si, sectors_error_msg
    call print_string
    jmp halt_system

halt_system:
    cli
    hlt
    jmp halt_system

REQUESTED_SECTORS  db 0
disk_error_msg     db "VenomOS: Disk read error, code: ", 0
sectors_error_msg  db "VenomOS: Incorrect sector count read", 13, 10, 0
