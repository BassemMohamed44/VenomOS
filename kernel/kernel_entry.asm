; ============================================================
; kernel_entry.asm - (64-bit revision)
;
; This is the very first code that runs inside the kernel image,
; now running in true 64-bit Long Mode (Stage 2 completed the
; Real Mode -> Protected Mode -> Long Mode transition and jumped
; here). Its only job is to set up a stack the C++ code can
; safely use, then call into kernel_main().
; ============================================================

BITS 64

global _start
extern kernel_main

section .text.entry

_start:
    ; ------------------------------------------------------------
    ; Set up a dedicated kernel stack (64-bit stack pointer RSP).
    ; Stacks grow downward on x86-64 too, so RSP starts at the
    ; *end* of the reserved region below.
    ; ------------------------------------------------------------
    mov rsp, kernel_stack_top

    ; ------------------------------------------------------------
    ; Hand off to C++. kernel_main() is declared extern "C" so its
    ; symbol name isn't mangled and this call resolves correctly.
    ; The System V AMD64 calling convention doesn't require any
    ; argument setup here since kernel_main() takes no parameters.
    ; ------------------------------------------------------------
    call kernel_main

    ; kernel_main() should never return, but if it somehow does,
    ; halt rather than execute whatever garbage comes next.
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kernel_stack_bottom:
    resb 16384          ; 16 KB kernel stack
kernel_stack_top:

; Tells the linker this object doesn't need an executable stack,
; silencing the "missing .note.GNU-stack section" warning.
section .note.GNU-stack noalloc noexec nowrite progbits
