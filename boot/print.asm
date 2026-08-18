print_string:
    pusha
    mov ah, 0x0E      
.loop:
    lodsb               
    cmp al, 0
    je .done
    int 0x10           
    jmp .loop
.done:
    popa
    ret


print_hex:
    pusha
    mov al, dl
    mov cl, 4
    shr al, cl    
    call print_hex_digit
    mov al, dl
    and al, 0x0F     
    call print_hex_digit
    popa
    ret


print_hex_digit:
    cmp al, 0x0A
    jl .is_digit
    add al, 'A' - 0x0A
    jmp .emit
.is_digit:
    add al, '0'
.emit:
    mov ah, 0x0E
    int 0x10
    ret
