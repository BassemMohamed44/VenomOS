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
