# VenomOS Architecture

This document describes VenomOS as it is implemented today. For the story of how each piece was built, including bugs found along the way, see [STEP-BY-STEP.md](STEP-BY-STEP.md). For what is not done yet, see [ROADMAP.md](ROADMAP.md).

## Overview

```
BIOS
 │
 ▼
Stage 1 ──► Stage 2 ──► Protected Mode ──► Long Mode
                                              │
                                              ▼
                                        kernel_main()
                                              │
   ┌─────────────┬──────────────┬─────────────┼───────────────┬──────────────┐
   ▼             ▼              ▼             ▼               ▼              ▼
 Drivers      Interrupts      Memory      Tasks and       Storage and     Shell and
 (VGA, PS/2)  (IDT, PIC,      (PMM,       processes       executables     applications
              syscalls)       paging,     (scheduler,     (ATA, VenomFS,  (shell, Snake,
                              heap)       ring 3)         ELF loader)     demo programs)
```

## 1. Boot

| Step | Where | What happens |
|------|-------|--------------|
| 1 | `boot/stage1.asm` | BIOS loads the 512-byte boot sector to `0x7C00`. It sets up segments and a stack, loads Stage 2 from disk, and jumps to it, passing the boot drive in `DL`. |
| 2 | `boot/stage2.asm` | Loads the kernel from disk into a temporary buffer. |
| 3 | `boot/cpu_check.asm` | Checks that the CPU supports CPUID and Long Mode. |
| 4 | `boot/mmap.asm` | Reads the physical memory map with BIOS `INT 15h, EAX=0xE820`. The entry count is stored at `0x8FF8` and the entries at `0x9000` for the kernel's PMM. |
| 5 | `boot/a20.asm`, `boot/gdt.asm` | Enables A20 and loads the GDT: 32-bit code and data, 64-bit ring 0 code, ring 3 code and data (selectors `0x23` and `0x2B` with RPL 3), and a 16-byte TSS descriptor. |
| 6 | `boot/stage2.asm` | Switches to Protected Mode and copies the kernel to `0x100000`. Builds identity-mapped page tables for the first 1 GiB using 2 MiB pages. Enables PAE, sets `EFER.LME` and `EFER.NXE`, enables paging, and far-jumps into 64-bit code. |
| 7 | `boot/stage2.asm` | Exports the TSS address, its GDT descriptor address and the TSS selector to a fixed location so the kernel can finish TSS setup at runtime. |
| 8 | `kernel/kernel_entry.asm` | Jumps to `kernel_main()`. |

The disk image is `stage1` (512 bytes) + `stage2` (exactly 3072 bytes) + the kernel padded to a fixed budget of 160 sectors (80 KiB), then extended to 16 MiB. The `Makefile` fails the build if any of these sizes is exceeded.

### `kernel_main()` sequence

1. Initialize VGA, keyboard and interrupts; enable interrupts; keep the real boot log visible for a few seconds.
2. `pmm::init()`, then self-tests for paging and syscall pointer validation.
3. `heap::init()`, `task::init()`, then self-tests for the task lifecycle and ring 3 stack selection.
4. `ata::init()`. If a drive is found, `fs::init()` mounts (or formats) VenomFS, `hello.elf` is written to it, and the ELF loader and filesystem error-handling self-tests run.
5. `shell::init()`, then the main loop (`hlt`, poll keyboard, `shell::tick()`, feed characters to the shell).

## 2. Interrupts and Exceptions (`kernel/interrupts.*`)

- 256 IDT gates, all pointing at assembly stubs (`interrupts.asm`) that build an `InterruptFrame` and call `interrupt_dispatch()`.
- The 8259 PIC is remapped to vectors 32-47. Only IRQ0 (timer) and IRQ1 (keyboard) are unmasked. The timer runs at the default PIT rate (about 18.2 ticks per second) and drives the scheduler. The ATA driver is polled and uses no IRQ.
- Vector `0x80` is a DPL 3 gate used for system calls.
- A `#GP` or page fault raised from ring 3 is reported, and the offending task is terminated with exit code `-1`. A page fault on a registered stack guard page is reported as a stack overflow. Any other CPU exception halts the machine.

## 3. Memory

### Physical memory (`kernel/pmm.*`)

A bitmap allocator over 4 KiB frames. It is initialized from the `E820` map, reserves low memory and the kernel image, and manages only the first 1 GiB (`MANAGED_LIMIT = 0x40000000`). API: `alloc_frame()`, `free_frame()`, and total / used / free counters (shown by `meminfo`).

### Virtual memory (`kernel/paging.*`)

- The boot page tables identity-map the first 1 GiB with 2 MiB pages. Kernel code, data, stacks, heap, page tables and device memory live there and stay reachable from every address space.
- `map_page`, `unmap_page` and `protect_page` operate on the current address space. `map_page_in(pml4, ...)` maps into any PML4. Flags: `PRESENT`, `WRITABLE`, `USER`, `NX`, and a software `OWNED` bit (bit 9) marking frames that belong to the address space and are freed with it. Mapping a 4 KiB page inside a range covered by a huge page is refused.
- **Per-process address spaces**: `create_address_space()` allocates a fresh PML4 and PDPT. PDPT slot 0 shares the kernel's identity-map page directory; every other slot starts empty and holds process-private mappings. `destroy_address_space()` frees every private frame and never touches the shared kernel mapping.

### Kernel heap (`kernel/heap.*`)

`kmalloc` / `kfree` over a free list with 16-byte alignment, block splitting, and merging of adjacent free blocks. The heap grows by taking frames from the PMM. Blocks carry magic values so double frees and corrupted headers are detected and reported.

## 4. Tasks, Processes and Scheduling (`kernel/task.*`, `kernel/scheduler.*`)

Every task is a `task::Task` control block with a PID (index plus generation, so stale PIDs are detected), parent PID, child links, exit code, saved `rsp`, a 16 KiB kernel stack, a `cr3`, and the user entry point and stack top for processes.

```
NEW -> READY -> RUNNING -> { BLOCKED | SLEEPING } -> READY -> ...
                    │
                    ▼
              ZOMBIE -> DEAD   (straight to DEAD if orphaned)
```

- **Table**: 128 task slots, plus a permanent idle task (`sti; hlt`) that runs when nothing else is ready.
- **Scheduler**: the timer interrupt calls `scheduler::tick()`, which wakes sleeping tasks whose time has come and performs a round-robin switch every 5 ticks. `yield()` reschedules immediately. There are no priorities.
- **Address spaces**: `create()` makes a kernel thread (`cr3 = 0`, shares the kernel address space). `create_process()` takes a private PML4. `switch_to()` reloads `CR3` only when the incoming task's address space differs from the one loaded.
- **Kernel stack on ring 3 entry**: on each switch to a task that has a kernel stack, `TSS.RSP0` is set to the top of that task's own stack.
- **Lifecycle**: `block_current()` / `unblock()`, `sleep_current()`, `exit_current()`, and `wait_for_child()`. On exit a task frees its stack and private address space; if its parent is alive it becomes a Zombie until the parent reaps it, otherwise it goes straight to Dead.
- **No synchronization primitives** (mutex, semaphore) exist yet.

## 5. User Mode and System Calls (`kernel/ring3.*`, `kernel/usercopy.*`)

- Ring 3 uses selectors `0x23` (code) and `0x2B` (data). `ring3::enter(entry, stack_top)` builds an `IRETQ` frame and drops to CPL 3.
- **Calling convention**: `int 0x80`, syscall number in `rax`, arguments in `rdi` and `rsi`, result in `rax`.

| Number | Name | Behavior |
|--------|------|----------|
| 0 | `SYS_EXIT` | `rdi` = exit code; terminates the process |
| 1 | `SYS_WRITE` | `rdi` = pointer, `rsi` = length (max 1 MiB); copies from user memory in 256-byte chunks and prints; returns bytes written or `-1` |
| 2 | `SYS_SLEEP` | `rdi` = ticks; sleeps the caller; returns `0` |

Unknown numbers return `-1`.

- **Pointer validation**: `usercopy::copy_from_user` / `copy_to_user` walk the current page tables and require every page in the range to be present and user-accessible (and writable for writes) before touching it. A boot self-test exercises the rejection paths.
- **Demos**: `usermode` runs a flat ring 3 program that prints via `SYS_WRITE` and then executes `cli` to trigger a real `#GP`. `stackoverflow` runs one that pushes onto its stack in an endless loop until it hits the unmapped guard page.

## 6. Storage

### ATA driver (`kernel/ata.*`)

Polling PIO driver for the primary channel's master drive. `init()` issues `IDENTIFY` to confirm a drive exists. 512-byte sectors, LBA addressing.

### VenomFS (`kernel/fs.*`)

A small custom filesystem: flat namespace, up to 64 files, 31-character names, and up to 4 extents (runs of consecutive sectors) per file.

| Region | LBA | Size | Contents |
|--------|-----|------|----------|
| Superblock | 256 | 1 sector | Magic `VNFS`, version 2, layout |
| File table | 257 | 9 sectors | 64 entries of 72 bytes (name, size, extent count, used flag, extents) |
| Free bitmap | 266 | 8 sectors | One bit per data sector |
| Data | 274 onward | to sector 32768 | File contents |

`fs::init()` formats the disk automatically if the superblock magic is missing. The API is `exists`, `file_size`, `read`, `write` (create or overwrite), `remove` and `entry_at`.

## 7. Executables and Processes (`kernel/elf.*`, `kernel/process.*`)

`process::run(filename)`:

1. Read the file from VenomFS (at most 64 KiB).
2. `elf::validate()` checks, among other things: ELF magic, 64-bit little-endian x86-64, `ET_EXEC` only, program header table bounds and size, at most 16 `PT_LOAD` segments, supported header types only (`PT_LOAD`, `PT_NULL`, `PT_GNU_STACK`), no segment that is both writable and executable, `p_memsz >= p_filesz`, no offset or address overflow, segments inside user space (at or above `0x40000000`, below the canonical limit), valid alignment, no overlapping segments, and an entry point inside a loaded segment.
3. Create a new address space and map every `PT_LOAD` segment page by page: frames are zero-filled, `p_filesz` bytes are copied in (the rest is `.bss`), and permissions come from `p_flags` (`USER` + `OWNED`, writable only if `PF_W`, `NX` unless `PF_X`).
4. Map a 16 KiB user stack at `0x7FFFF000`, with an unmapped guard page just below it at `0x7FFFE000`.
5. `task::create_process()` with the ELF's real `e_entry`. When scheduled, the task sets up the TSS and enters ring 3.

Any failure frees what was allocated and reports a specific error message.

## 8. Drivers and Applications

| Component | Files | Notes |
|-----------|-------|-------|
| VGA text terminal | `vga.*` | 80x25, colors, scrolling, hardware cursor |
| PS/2 keyboard | `keyboard.*` | Scancode translation, shift, caps lock, extended scancodes |
| Shell | `shell.*` | Command list in the [README](../README.md#shell-commands) |
| Snake | `snake.*` | Runs inside the shell using the keyboard and VGA drivers |

## 9. Source Map

| Area | Files |
|------|-------|
| Bootloader | `boot/*.asm` |
| Entry and sequence | `kernel/kernel_entry.asm`, `kernel/kernel.cpp` |
| Interrupts and syscalls | `kernel/interrupts.*` |
| Memory | `kernel/pmm.*`, `kernel/paging.*`, `kernel/heap.*` |
| Tasks and scheduling | `kernel/task.*`, `kernel/scheduler.*`, `kernel/task_switch.asm` |
| User mode | `kernel/ring3.*`, `kernel/usercopy.*`, `kernel/user_program*.asm`, `kernel/stack_overflow_program*.asm` |
| Storage | `kernel/ata.*`, `kernel/fs.*` |
| Executables | `kernel/elf.*`, `kernel/process.*`, `kernel/demo_elf.asm`, `kernel/user_link.ld` |
| Drivers and apps | `kernel/vga.*`, `kernel/keyboard.*`, `kernel/shell.*`, `kernel/snake.*` |
| Shared headers | `include/io.hpp`, `include/stdint.hpp`, `include/stddef.hpp` |

## 10. Limitations

Summarized from the code; the full list lives in the [README](../README.md#known-limitations).

- Single core, round-robin only, no priorities, no synchronization primitives.
- Memory management covers the first 1 GiB of RAM only.
- Three syscalls; only the ring 0 shell can start processes; `wait` reaps any child.
- Static `ET_EXEC` ELF64 only, 64 KiB maximum, fixed 16 KiB stack.
- VenomFS is flat, with 64 files and at most 4 extents per file, on a fixed 16 MiB disk.
- PIC, PS/2 and VGA text mode only.
