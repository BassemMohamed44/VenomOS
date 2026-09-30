check_cpu_supported:

    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    mov eax, 1
    cpuid
    test edx, 1 << 6
    jz .no_pae
    test edx, 1 << 5
    jz .no_msr

    ret

.no_cpuid:
    mov si, msg_no_cpuid
    jmp .fail

.no_long_mode:
    mov si, msg_no_long_mode
    jmp .fail

.no_pae:
    mov si, msg_no_pae
    jmp .fail

.no_msr:
    mov si, msg_no_msr
    jmp .fail

.fail:
    call print_string
    mov si, msg_unsupported_halt
    call print_string
    cli
.halt_forever:
    hlt
    jmp .halt_forever

msg_no_cpuid        db "VenomOS: CPU error - CPUID instruction not supported.", 13, 10, 0
msg_no_long_mode    db "VenomOS: CPU error - 64-bit Long Mode not supported.", 13, 10, 0
msg_no_pae          db "VenomOS: CPU error - PAE not supported.", 13, 10, 0
msg_no_msr          db "VenomOS: CPU error - MSRs not supported.", 13, 10, 0
msg_unsupported_halt db "VenomOS: Unsupported CPU. System halted.", 13, 10, 0
