# Roadmap

The status below reflects the code in the repository. Architecture details are in [ARCHITECTURE.md](ARCHITECTURE.md).

## Completed

- Bootloader (Stage 1 + Stage 2, A20, GDT, Protected Mode, Long Mode, NX enabled)
- CPU feature check and BIOS `E820` memory map detection
- Freestanding C++17 kernel, VGA driver, PS/2 keyboard driver
- Interrupts: IDT, remapped PIC, timer, keyboard, exception handling
- Physical Memory Manager (bitmap, `E820`-driven)
- Paging with page protection, NX, and **per-process address spaces**
- Kernel heap (`kmalloc` / `kfree`, split / merge, corruption detection)
- Preemptive round-robin scheduler and multitasking
- Process lifecycle: PID, parent / child, 7-state machine, exit codes, zombie reaping, sleep, idle task
- Ring 3 user mode, TSS with per-task kernel stack, `int 0x80` syscalls (`exit`, `write`, `sleep`)
- Syscall pointer validation (`usercopy`)
- ATA PIO driver
- VenomFS persistent filesystem (custom, not FAT) with multi-extent files and automatic formatting
- **ELF64 loader** with hardened validation (`W^X`, bounds and overlap checks, user-space range checks) and NX applied to non-executable segments
- Interactive shell, including `ps`, `wait`, `sleep`, `run`, `meminfo`, `memtest`
- Snake game (bonus)
- Boot-time self-tests (paging, usercopy, task lifecycle, ring 3, ELF loader, filesystem)

## In Progress

- Documentation alignment with the implemented architecture (this update)

## Planned

**Processes and syscalls**

- More syscalls: `read`, file access, `getpid`
- Let ring 3 processes spawn children (today only the ring 0 shell can)
- `wait` for a specific PID
- Synchronization primitives (mutex, semaphore)
- Priorities or a smarter scheduler

**Executables**

- PIE / dynamic linking support
- A host-side tool to put programs on the disk image

**Storage**

- Directories and chained (non-contiguous) block allocation in VenomFS
- A standard filesystem such as FAT32

**Memory**

- Manage more than the first 1 GiB of RAM

**Hardware**

- APIC and SMP
- Networking

**Project hygiene**

- Make the CI workflow run: move it from `example.github/` to `.github/` and install the `x86_64-elf` cross-compiler
- Remove the duplicate `docs/step-by-step.md` (an older copy that collides with `STEP-BY-STEP.md` on case-insensitive filesystems)

## Known Limitations

See [README.md](../README.md#known-limitations) for the full, categorized list.

## Active Issues

Open items are tracked in the GitHub issue tracker. The planned items above that are not obviously covered by an existing issue are good candidates to file.
