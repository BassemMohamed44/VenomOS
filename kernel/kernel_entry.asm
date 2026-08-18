BITS 64

global _start
extern kernel_main

section .text.entry

_start:

    mov rsp, kernel_stack_top

    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kernel_stack_bottom:
    resb 16384         
kernel_stack_top:


section .note.GNU-stack noalloc noexec nowrite progbits
