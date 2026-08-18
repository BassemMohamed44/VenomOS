BITS 16
ORG 0x8000


KERNEL_TEMP_SEGMENT equ 0x1000
KERNEL_TEMP_OFFSET  equ 0x0000
KERNEL_TEMP_LINEAR  equ 0x10000
KERNEL_SECTOR_COUNT equ 128                   
KERNEL_LOAD_ADDR    equ 0x100000             

STAGE2_SECTOR_COUNT equ 6

KERNEL_START_SECTOR equ 2 + STAGE2_SECTOR_COUNT 


PML4_ADDR equ 0x1000
PDPT_ADDR equ 0x2000
PD_ADDR   equ 0x3000


TSS_INFO_ADDR equ 0x8FE0

start:
    mov [BOOT_DRIVE], dl  

    mov si, msg_stage2
    call print_string


    call detect_memory_map
    mov si, msg_mmap
    call print_string
    mov dl, [E820_COUNT_ADDR]   
    call print_hex
    mov si, msg_mmap_entries
    call print_string

    mov si, msg_kernel_loading
    call print_string

    mov ax, KERNEL_TEMP_SEGMENT
    mov es, ax
    mov bx, KERNEL_TEMP_OFFSET
    mov al, KERNEL_SECTOR_COUNT
    mov cl, KERNEL_START_SECTOR
    mov dl, [BOOT_DRIVE]
    call disk_load

    mov si, msg_kernel_loaded
    call print_string

    call enable_a20
    mov si, msg_a20
    call print_string

    mov si, msg_gdt
    call print_string

    cli                     

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1             
    mov cr0, eax

    jmp CODE32_SEG:protected_mode_start

%include "print.asm"
%include "a20.asm"
%include "gdt.asm"
%include "disk.asm"
%include "mmap.asm"

BOOT_DRIVE          db 0
msg_stage2          db "VenomOS Stage2: Loaded successfully!", 13, 10, 0
msg_mmap             db "VenomOS Stage2: Memory map detected, entries: 0x", 0
msg_mmap_entries    db 13, 10, 0
msg_kernel_loading  db "VenomOS Stage2: Loading kernel from disk...", 13, 10, 0
msg_kernel_loaded   db "VenomOS Stage2: Kernel loaded into temporary buffer.", 13, 10, 0
msg_a20             db "VenomOS Stage2: A20 line enabled.", 13, 10, 0
msg_gdt             db "VenomOS Stage2: GDT loaded. Switching to Protected Mode...", 13, 10, 0

BITS 32

protected_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000    

    mov esi, msg_pm32
    mov edi, 0xB8000
    mov ah, 0x0F
.pm32_print_loop:
    lodsb
    cmp al, 0
    je .pm32_print_done
    mov [edi], ax
    add edi, 2
    jmp .pm32_print_loop
.pm32_print_done:

    mov esi, KERNEL_TEMP_LINEAR
    mov edi, KERNEL_LOAD_ADDR
    mov ecx, (KERNEL_SECTOR_COUNT * 512) / 4
    rep movsd

    mov edi, PML4_ADDR
    xor eax, eax
    mov ecx, 3 * 4096 / 4       
    rep stosd

    mov dword [PML4_ADDR], PDPT_ADDR | 0x3     
    mov dword [PML4_ADDR + 4], 0

    mov dword [PDPT_ADDR], PD_ADDR | 0x3       
    mov dword [PDPT_ADDR + 4], 0

    mov edi, PD_ADDR
    mov eax, 0x83               
    mov ecx, 512
.fill_page_directory:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000            
    add edi, 8
    loop .fill_page_directory

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, PML4_ADDR
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    jmp CODE64_SEG:long_mode_start

msg_pm32 db "VenomOS: 32-bit Protected Mode active. Building page tables...", 0

BITS 64

long_mode_start:

    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, 0x00090000   

    mov rsi, msg_long_mode
    mov rdi, 0xB8000 + 160   
    mov ah, 0x0F
.lm_print_loop:
    lodsb
    cmp al, 0
    je .lm_print_done
    mov [rdi], ax
    add rdi, 2
    jmp .lm_print_loop
.lm_print_done:

    mov rax, tss_start
    mov [TSS_INFO_ADDR], rax
    mov rax, gdt_tss
    mov [TSS_INFO_ADDR + 8], rax
    mov rax, TSS_SEG
    mov [TSS_INFO_ADDR + 16], rax

    jmp KERNEL_LOAD_ADDR

msg_long_mode db "VenomOS: 64-bit Long Mode active! Jumping to kernel...", 0

times 3072-($-$$) db 0
