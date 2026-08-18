BITS 64

global _start

section .text

_start:
    lea rdi, [rel msg]
    mov rsi, msg_len
    mov rax, 1        
    int 0x80

    mov rdi, 42         
    mov rax, 0          
    int 0x80

.unreachable:
    jmp .unreachable

section .rodata

msg: db "Hello from a genuine ELF64 executable - parsed, loaded, and run by VenomOS's own ELF loader!", 10, 0
msg_len equ $ - msg - 1

section .note.GNU-stack noalloc noexec nowrite progbits
