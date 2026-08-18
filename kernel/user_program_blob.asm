section .rodata

global user_program_start
global user_program_end

user_program_start:
    incbin "build/user_program.bin"
user_program_end:

section .note.GNU-stack noalloc noexec nowrite progbits
