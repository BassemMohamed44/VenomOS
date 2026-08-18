disk_load:
    mov [REQUESTED_SECTORS], al   

    mov ah, 0x02        
    mov ch, 0x00        
    mov dh, 0x00        
    int 0x13

    jc disk_error       

    cmp al, [REQUESTED_SECTORS]  
    jne sectors_error
    ret

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

REQUESTED_SECTORS  db 0
disk_error_msg     db "VenomOS: Disk read error, code: ", 0
sectors_error_msg  db "VenomOS: Incorrect sector count read", 13, 10, 0
