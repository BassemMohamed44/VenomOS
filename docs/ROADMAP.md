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
- Ring 3 user mode, TSS with per-task kernel stack, `int 0x80` syscalls (`exit`, `write`, `sleep`, `getpid`, `getppid`, `uptime`, `read`, `filesize`, `readfile`)
- `W^X` memory protection: NX on user data/stack and the kernel heap, kernel image split into R+X / R / RW+NX pages, `CR0.WP`
- System V `argc`/`argv`/`envp` entry stack for ELF processes (`run <file> [args...]`)
- Scheduler stress tests (`stress [full]`) and a clean-build workflow (`make check-clean`)
- Syscall pointer validation (`usercopy`)
- ATA PIO driver
- VenomFS persistent filesystem (custom, not FAT) with multi-extent files and automatic formatting
- Read-only FAT32 driver and a host tool (`tools/mkfat32.py`) that builds the volume; host-side tests via `make test-fat32`
- **ELF64 loader** with hardened validation (`W^X`, bounds and overlap checks, user-space range checks) and NX applied to non-executable segments
- Interactive shell, including `ps`, `wait`, `sleep`, `run`, `meminfo`, `memtest`
- Snake game (bonus)
- Boot-time self-tests (paging, usercopy, task lifecycle, ring 3, ELF loader, filesystem)

## In Progress

- Nothing in progress. See the open issues in the tracker.

## Planned

**Processes and syscalls**

- Blocking input, file descriptors, `spawn` / `wait` for ring 3
- Let ring 3 processes spawn children (today only the ring 0 shell can)
- `wait` for a specific PID
- Synchronization primitives (mutex, semaphore)
- Priorities or a smarter scheduler

**Executables**

- PIE / dynamic linking support
- A host-side tool to put programs on the disk image

**Storage**

- Directories and chained (non-contiguous) block allocation in VenomFS
- FAT32: long file names, write support, and running programs from it

**Memory**

- Manage more than the first 1 GiB of RAM

**Hardware**

- APIC and SMP
- Networking

**Project hygiene**

- Make the CI workflow run: move it from `example.github/` to `.github/` and install the `x86_64-elf` cross-compiler

## Known Limitations

See [README.md](../README.md#known-limitations) for the full, categorized list.

## Active Issues

Open items are tracked in the GitHub issue tracker. The planned items above that are not obviously covered by an existing issue are good candidates to file.
