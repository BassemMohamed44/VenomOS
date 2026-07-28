; ============================================================
; print.asm
; BIOS teletype text output routines (16-bit real mode only)
; ============================================================

; print_string
; Prints a null-terminated string using BIOS teletype (int 10h, ah=0eh)
; Input : DS:SI -> pointer to null-terminated string
; Clobbers: none (all registers preserved)
print_string:
    pusha
    mov ah, 0x0E        ; BIOS teletype output function
.loop:
    lodsb               ; AL = [DS:SI], SI++
    cmp al, 0
    je .done
    int 0x10            ; print character in AL
    jmp .loop
.done:
    popa
    ret

; print_hex
; Prints the byte in DL as two hexadecimal digits (e.g. 0x1B -> "1B")
; Input : DL = byte to print
; Clobbers: none (all registers preserved)
print_hex:
    pusha
    mov al, dl
    mov cl, 4
    shr al, cl          ; high nibble
    call print_hex_digit
    mov al, dl
    and al, 0x0F         ; low nibble
    call print_hex_digit
    popa
    ret

; print_hex_digit (internal helper)
; Input: AL = value 0-15
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
