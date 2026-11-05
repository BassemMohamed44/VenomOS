BITS 64

global _start

section .text

; Spawns fat:HELLO.ELF with the arguments "from spawn", waits for it, prints a
; line and exits with the child's exit code (42 for the demo program).
_start:
    lea rdi, [rel msg_start]
    mov rsi, msg_start_len
    mov rax, 1
    int 0x80

    lea rdi, [rel child_path]
    lea rsi, [rel child_args]
    mov rax, 9              ; SYS_SPAWN
    int 0x80
    cmp rax, -1
    je .failed

    sub rsp, 16
    mov rdi, rsp            ; &status
    mov rax, 10             ; SYS_WAIT
    int 0x80
    cmp rax, -1
    je .failed

    lea rdi, [rel msg_done]
    mov rsi, msg_done_len
    mov rax, 1
    int 0x80

    mov edi, [rsp]          ; child's exit code
    mov rax, 0
    int 0x80

.failed:
    lea rdi, [rel msg_fail]
    mov rsi, msg_fail_len
    mov rax, 1
    int 0x80
    mov rdi, 1
    mov rax, 0
    int 0x80

.unreachable:
    jmp .unreachable

section .rodata

child_path: db "fat:HELLO.ELF", 0
child_args: db "from spawn", 0
msg_start: db "spawner: starting a child...", 10
msg_start_len equ $ - msg_start
msg_done: db "spawner: child finished.", 10
msg_done_len equ $ - msg_done
msg_fail: db "spawner: spawn or wait failed.", 10
msg_fail_len equ $ - msg_fail

section .note.GNU-stack noalloc noexec nowrite progbits
