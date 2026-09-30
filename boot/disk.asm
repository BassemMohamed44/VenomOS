disk_load:
    mov [REQUESTED_SECTORS], al
    mov [DISK_DRIVE_NUM], dl

    mov [DEST_OFFSET], bx

    mov al, cl
    xor ah, ah
    dec ax
    mov [CURRENT_LBA], ax
    mov word [CURRENT_LBA + 2], 0

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [DISK_DRIVE_NUM]
    int 0x13
    jc no_extensions_error
    cmp bx, 0xAA55
    jne no_extensions_error

    mov al, [REQUESTED_SECTORS]
    mov [SECTORS_REMAINING], al

.read_chunk:
    cmp byte [SECTORS_REMAINING], 0
    je .all_done

    mov al, [SECTORS_REMAINING]
    cmp al, MAX_SECTORS_PER_READ
    jbe .chunk_size_ok
    mov al, MAX_SECTORS_PER_READ
.chunk_size_ok:
    mov [DAP_SECTOR_COUNT], al

    mov ax, [DEST_OFFSET]
    mov [DAP_BUFFER_OFFSET], ax
    mov ax, es
    mov [DAP_BUFFER_SEGMENT], ax

    mov ax, [CURRENT_LBA]
    mov [DAP_LBA_LOW], ax
    mov ax, [CURRENT_LBA + 2]
    mov [DAP_LBA_HIGH], ax

    mov byte [RETRY_COUNT], 3
.attempt_read:
    mov si, disk_address_packet
    mov ah, 0x42
    mov dl, [DISK_DRIVE_NUM]
    int 0x13
    jnc .chunk_ok

    dec byte [RETRY_COUNT]
    jnz .attempt_read
    jmp disk_error

.chunk_ok:

    movzx ax, byte [DAP_SECTOR_COUNT]
    add [CURRENT_LBA], ax
    adc word [CURRENT_LBA + 2], 0

    mov al, [DAP_SECTOR_COUNT]
    sub [SECTORS_REMAINING], al

    movzx ax, byte [DAP_SECTOR_COUNT]
    mov cx, 512
    mul cx
    add [DEST_OFFSET], ax

    jmp .read_chunk

.all_done:

    cmp byte [SECTORS_REMAINING], 0
    jne sectors_error

    clc
    ret

no_extensions_error:
    mov si, no_ext_error_msg
    call print_string
    jmp halt_system

disk_error:
    mov si, disk_error_msg
    call print_string
    mov dl, ah
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

disk_address_packet:
    db 0x10
    db 0x00
DAP_SECTOR_COUNT:
    db 0
    db 0
DAP_BUFFER_OFFSET:
    dw 0
DAP_BUFFER_SEGMENT:
    dw 0
DAP_LBA_LOW:
    dw 0
    dw 0
DAP_LBA_HIGH:
    dw 0
    dw 0

MAX_SECTORS_PER_READ equ 64

REQUESTED_SECTORS  db 0
SECTORS_REMAINING  db 0
RETRY_COUNT        db 0
DISK_DRIVE_NUM     db 0
CURRENT_LBA        dd 0
DEST_OFFSET        dw 0

disk_error_msg     db "Disk err: ", 0
sectors_error_msg  db "Sector cnt err", 13, 10, 0
no_ext_error_msg   db "No LBA ext", 13, 10, 0
