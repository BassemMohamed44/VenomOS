BITS 64
ORG 0x40021000

_user_start:
.loop:
    push rax
    jmp .loop
