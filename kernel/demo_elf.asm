BITS 64

global _start

section .text

_start:
    ; System V entry ABI: [rsp] = argc, [rsp+8] = argv[0] ...
    mov rbx, [rsp]
    lea r12, [rsp + 8]

    lea rdi, [rel msg]
    mov rsi, msg_len
    mov rax, 1
    int 0x80

    ; print every argv[i] on its own line
    xor r13, r13
.arg_loop:
    cmp r13, rbx
    jae .args_done
    mov rdi, [r12 + r13*8]
    xor rsi, rsi
.strlen:
    cmp byte [rdi + rsi], 0
    je .have_len
    inc rsi
    jmp .strlen
.have_len:
    mov rax, 1
    int 0x80
    lea rdi, [rel newline]
    mov rsi, 1
    mov rax, 1
    int 0x80
    inc r13
    jmp .arg_loop
.args_done:

    mov rdi, 42
    mov rax, 0
    int 0x80

.unreachable:
    jmp .unreachable

section .rodata

msg: db "Hello from a genuine ELF64 executable - parsed, loaded, and run by VenomOS's own ELF loader!", 10, 0
msg_len equ $ - msg - 1
newline: db 10

section .note.GNU-stack noalloc noexec nowrite progbits
