BITS 64

global switch_context

section .text

switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq

    mov [rdi], rsp     

    mov rsp, rsi         

    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret

section .note.GNU-stack noalloc noexec nowrite progbits
