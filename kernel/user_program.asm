BITS 64
ORG 0x40001000

_user_start:
    lea rdi, [rel msg]
    mov rsi, msg_len
    mov rax, 1            
    int 0x80

    cli

.unreachable:
    jmp .unreachable

msg: db "Hello from Ring 3! This line was printed via a real syscall (int 0x80).", 10, 0
msg_len equ $ - msg - 1
