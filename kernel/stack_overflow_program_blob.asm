section .rodata

global stack_overflow_program_start
global stack_overflow_program_end

stack_overflow_program_start:
    incbin "build/stack_overflow_program.bin"
stack_overflow_program_end:

section .note.GNU-stack noalloc noexec nowrite progbits
