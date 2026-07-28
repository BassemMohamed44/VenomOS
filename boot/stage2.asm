; ============================================================
; stage2.asm -(64-bit revision)
;
; Loaded by Stage 1 at 0x0000:0x8000 (linear address 0x8000),
; still running in 16-bit Real Mode at this point. DL holds the
; boot drive number, passed forward from Stage 1.
;
; Full responsibility chain of Stage 2:
;   1. Prove we were loaded correctly
;   2. Load the C++ kernel binary off disk into a temporary low
;      memory buffer (BIOS disk reads only work in Real Mode)
;   3. Enable the A20 line
;   4. Build/load a GDT (with both a 32-bit AND a 64-bit code
;      segment descriptor)
;   5. Switch to 32-bit Protected Mode (a mandatory intermediate
;      step - x86 CPUs cannot jump from Real Mode straight to
;      Long Mode)
;   6. In 32-bit code: relocate the kernel to its real load
;      address (0x100000), then build a minimal set of identity-
;      mapped page tables (Long Mode REQUIRES paging to be active,
;      unlike Protected Mode, where it's optional)
;   7. Enable PAE, enable Long Mode in the EFER MSR, enable paging
;   8. Far-jump into genuine 64-bit Long Mode
;   9. Jump to the 64-bit kernel's entry point at 0x100000
; ============================================================

BITS 16
ORG 0x8000

; ------------------------------------------------------------
; Kernel loading parameters
; ------------------------------------------------------------
KERNEL_TEMP_SEGMENT equ 0x1000
KERNEL_TEMP_OFFSET  equ 0x0000
KERNEL_TEMP_LINEAR  equ 0x10000
KERNEL_SECTOR_COUNT equ 64                     ; budget: 64 sectors = 32KB for the kernel image
KERNEL_LOAD_ADDR    equ 0x100000               ; must match kernel/linker.ld

; Must match the STAGE2_SECTOR_COUNT used in boot/stage1.asm and the
; Makefile's size check - this constant only exists here so
; KERNEL_START_SECTOR can be computed without hardcoding the number twice.
; NOTE: must be defined BEFORE KERNEL_START_SECTOR below - NASM's equ
; constants must be defined before they're referenced, unlike labels.
STAGE2_SECTOR_COUNT equ 6

KERNEL_START_SECTOR equ 2 + STAGE2_SECTOR_COUNT ; kernel starts right after stage2 on disk

; ------------------------------------------------------------
; Physical addresses for our identity-mapped page tables.
; This memory (0x1000-0x3FFF) is unused low memory, safely below
; Stage 1 (0x7C00) - reusing it is fine since by the time we build
; these tables, Real Mode's IVT/BDA at 0x0000-0x04FF are no longer
; needed (interrupts are already disabled for good).
; ------------------------------------------------------------
PML4_ADDR equ 0x1000
PDPT_ADDR equ 0x2000
PD_ADDR   equ 0x3000

start:
    mov [BOOT_DRIVE], dl    ; save the boot drive Stage 1 passed us in DL

    mov si, msg_stage2
    call print_string

    ; ------------------------------------------------------------
    ; Load the kernel image off disk into the temporary buffer
    ; ------------------------------------------------------------
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

    cli                     ; interrupts stay off from here on (Phase 5 sets up a real IDT)

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1             ; CR0.PE - Protection Enable
    mov cr0, eax

    jmp CODE32_SEG:protected_mode_start

; ------------------------------------------------------------
; Shared 16-bit routines (used only before the mode switch above)
; ------------------------------------------------------------
%include "print.asm"
%include "a20.asm"
%include "gdt.asm"
%include "disk.asm"

; ------------------------------------------------------------
; 16-bit data
; ------------------------------------------------------------
BOOT_DRIVE          db 0
msg_stage2          db "VenomOS Stage2: Loaded successfully!", 13, 10, 0
msg_kernel_loading  db "VenomOS Stage2: Loading kernel from disk...", 13, 10, 0
msg_kernel_loaded   db "VenomOS Stage2: Kernel loaded into temporary buffer.", 13, 10, 0
msg_a20             db "VenomOS Stage2: A20 line enabled.", 13, 10, 0
msg_gdt             db "VenomOS Stage2: GDT loaded. Switching to Protected Mode...", 13, 10, 0

; ============================================================
; 32-bit Protected Mode code. This is only a brief waypoint on
; the way to Long Mode - it relocates the kernel, builds page
; tables, then switches again.
; ============================================================
BITS 32

protected_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000     ; temporary 32-bit stack, used only until Long Mode sets its own

    ; ------------------------------------------------------------
    ; Prove we made it into Protected Mode by writing directly to
    ; the VGA text buffer (BIOS int 10h no longer works here).
    ; ------------------------------------------------------------
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

    ; ------------------------------------------------------------
    ; Relocate the kernel from its temporary buffer (0x10000) to
    ; its real linked load address (0x100000). Paging is not yet
    ; enabled, so these are plain physical addresses.
    ; ------------------------------------------------------------
    mov esi, KERNEL_TEMP_LINEAR
    mov edi, KERNEL_LOAD_ADDR
    mov ecx, (KERNEL_SECTOR_COUNT * 512) / 4
    rep movsd

    ; ------------------------------------------------------------
    ; Build minimal identity-mapped page tables for Long Mode.
    ; Long Mode requires paging to be active (unlike Protected
    ; Mode, where it's optional) - so this step is mandatory, not
    ; a Phase 4 nice-to-have. We map the first 1GB of physical
    ; memory 1:1 using 512 x 2MB pages at the Page Directory level,
    ; which lets us skip building a separate Page Table level
    ; entirely (2MB "huge pages").
    ;
    ; Layout: PML4 (0x1000) -> PDPT (0x2000) -> PD (0x3000, 512
    ; entries of 2MB each = 1GB covered, comfortably including our
    ; kernel at 0x100000).
    ; ------------------------------------------------------------
    mov edi, PML4_ADDR
    xor eax, eax
    mov ecx, 3 * 4096 / 4       ; zero all three 4KB tables (3 dwords-worth)
    rep stosd

    mov dword [PML4_ADDR], PDPT_ADDR | 0x3     ; present + writable
    mov dword [PML4_ADDR + 4], 0

    mov dword [PDPT_ADDR], PD_ADDR | 0x3       ; present + writable
    mov dword [PDPT_ADDR + 4], 0

    mov edi, PD_ADDR
    mov eax, 0x83               ; present + writable + PS (2MB page), base address 0
    mov ecx, 512
.fill_page_directory:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000            ; next 2MB physical frame (low 12 bits untouched)
    add edi, 8
    loop .fill_page_directory

    ; ------------------------------------------------------------
    ; Enable PAE (Physical Address Extension) - CR4 bit 5.
    ; Long Mode paging is always at least PAE-style paging.
    ; ------------------------------------------------------------
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Point CR3 at our PML4 table.
    mov eax, PML4_ADDR
    mov cr3, eax

    ; ------------------------------------------------------------
    ; Set the Long Mode Enable (LME) bit in the EFER MSR
    ; (Model-Specific Register 0xC0000080, bit 8).
    ; ------------------------------------------------------------
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; ------------------------------------------------------------
    ; Enable paging (CR0 bit 31). With LME already set, this single
    ; instruction activates IA-32e "compatibility mode" - the CPU
    ; is running 64-bit paging but still executing our 32-bit code
    ; segment. The far jump below is what completes the transition
    ; into true 64-bit Long Mode.
    ; ------------------------------------------------------------
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    jmp CODE64_SEG:long_mode_start

msg_pm32 db "VenomOS: 32-bit Protected Mode active. Building page tables...", 0

; ============================================================
; True 64-bit Long Mode code.
; ============================================================
BITS 64

long_mode_start:
    ; Reload segment registers - in Long Mode the CPU ignores
    ; most of their base/limit fields, but a valid selector must
    ; still be loaded (particularly for SS).
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, 0x00090000     ; temporary 64-bit stack, used only until the kernel sets its own

    ; Prove we're in real 64-bit Long Mode via direct VGA write.
    mov rsi, msg_long_mode
    mov rdi, 0xB8000 + 160   ; second screen row, so it doesn't overwrite the PM32 message
    mov ah, 0x0F
.lm_print_loop:
    lodsb
    cmp al, 0
    je .lm_print_done
    mov [rdi], ax
    add rdi, 2
    jmp .lm_print_loop
.lm_print_done:

    ; Everything is in place - jump to the 64-bit kernel's entry point.
    jmp KERNEL_LOAD_ADDR

msg_long_mode db "VenomOS: 64-bit Long Mode active! Jumping to kernel...", 0

; ------------------------------------------------------------
; Pad Stage 2 out to a whole number of sectors. Must match
; STAGE2_SECTOR_COUNT * 512 in stage1.asm and above (6 * 512 = 3072).
; ------------------------------------------------------------
times 3072-($-$$) db 0
