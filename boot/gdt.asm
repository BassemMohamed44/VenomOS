; ============================================================
; gdt.asm - (64-bit revision)
;
; The Global Descriptor Table (GDT) tells the CPU what memory
; segments exist. We need THREE usable descriptors now instead
; of two:
;   1. A flat 32-bit code segment - used only briefly, as the
;      landing pad right after enabling Protected Mode, before
;      we've set up paging and can enter Long Mode.
;   2. A flat data segment - reused for DS/ES/SS/etc in both
;      32-bit Protected Mode and 64-bit Long Mode (in Long Mode
;      the CPU ignores most of its base/limit fields anyway, so
;      one flat descriptor safely covers both cases).
;   3. A 64-bit ("Long Mode") code segment - its base/limit
;      fields are entirely ignored by the CPU; what matters is
;      the L-bit (bit 21 overall / bit 5 of the flags nibble),
;      which tells the CPU "run this segment's code as 64-bit".
; ============================================================

gdt_start:

; ------------------------------------------------------------
; Null descriptor - required by the x86 architecture.
; ------------------------------------------------------------
gdt_null:
    dq 0x0

; ------------------------------------------------------------
; 32-bit code segment descriptor - selector 0x08
; Base = 0, Limit = 0xFFFFF (4K granularity -> 4GB flat)
; Access 10011010b: present, ring0, code, executable, readable
; Flags 1100b: granularity=1 (4K), size=1 (32-bit segment)
; ------------------------------------------------------------
gdt_code32:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

; ------------------------------------------------------------
; Flat data segment descriptor - selector 0x10
; Access 10010010b: present, ring0, data, writable
; Reused for DS/ES/SS/FS/GS in both Protected Mode and Long Mode.
; ------------------------------------------------------------
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

; ------------------------------------------------------------
; 64-bit ("Long Mode") code segment descriptor - selector 0x18
; Base/limit are ignored by the CPU in 64-bit mode, but we fill
; them the conventional way (0 base, max limit) for clarity.
; Access 10011010b: same meaning as the 32-bit code descriptor.
; Flags byte 0xAF = 10101111b:
;   bit7 G=1 (granularity, irrelevant here but conventional)
;   bit6 D/B=0 (MUST be 0 when L=1 - the L-bit takes over its role)
;   bit5 L=1  <- THIS is what makes it a 64-bit code segment
;   bit4 AVL=0
;   bits3-0 = limit bits 16-19 (irrelevant, set to 1111 by convention)
; ------------------------------------------------------------
gdt_code64:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 10101111b
    db 0x00

gdt_end:

; ------------------------------------------------------------
; GDT descriptor (GDTR contents) - what LGDT actually loads.
; ------------------------------------------------------------
gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; size of GDT, one byte less than actual size
    dd gdt_start                ; linear address of the GDT itself

; ------------------------------------------------------------
; Segment selectors - byte offsets into the GDT.
; ------------------------------------------------------------
CODE32_SEG equ gdt_code32 - gdt_start
DATA_SEG   equ gdt_data   - gdt_start
CODE64_SEG equ gdt_code64 - gdt_start
