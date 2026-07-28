/* ============================================================
 * linker.ld - (64-bit revision)
 *
 * Links the kernel as a flat, position-dependent 64-bit ELF image
 * loaded at physical/linear address 0x100000 (1MB). Stage 2 already
 * built identity-mapped page tables covering the first 1GB before
 * jumping here, so linear addresses ARE physical addresses for
 * everything in that range - whatever address we link against is
 * exactly where Stage 2 must place the kernel bytes in memory.
 * ============================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LOAD_ADDR = 0x100000;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    /* kernel_entry.asm's stub MUST be the very first bytes of the
       image, since Stage 2 jumps straight to KERNEL_LOAD_ADDR with
       no ELF parsing - it has no idea where kernel_main lives. */
    .text.entry : {
        *(.text.entry)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* Marks the first free byte after the kernel image - Phase 4's
       physical memory manager and heap will start allocating from here. */
    kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
