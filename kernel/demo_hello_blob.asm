section .rodata

global demo_hello_start
global demo_hello_end

demo_hello_start:
    incbin "build/demo_hello.elf"
demo_hello_end:

section .note.GNU-stack noalloc noexec nowrite progbits
