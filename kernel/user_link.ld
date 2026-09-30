/* ============================================================
 * user_link.ld - VenomOS ELF Loader phase
 *
 * Links a user-space demo program (e.g. kernel/demo_elf.asm) into a
 * genuine ET_EXEC ELF64 executable, based at 0x40000000 (1GB).
 *
 * This MUST be >= 1GB, not the more traditional-looking 0x400000
 * (4MB): every process's address space "borrows" (shares) the
 * kernel's boot-time identity map for the first 1GB (see
 * paging::create_address_space()), which boot/stage2.asm built out
 * of 2MB huge pages - a 4KB PT_LOAD segment can't be mapped inside
 * that range at all (paging::map_page_in() correctly refuses rather
 * than silently corrupting a huge page). 1GB and above is each
 * process's own, private address range - exactly where a per-process
 * mapping needs to live.
 *
 * This is a completely different linker script from kernel/linker.ld
 * (which links the KERNEL itself) - this one produces the on-disk
 * executable kernel/process.cpp's ELF loader parses at runtime, not
 * something linked into the kernel image directly.
 * ============================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text)
        *(.text.*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
