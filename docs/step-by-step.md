# VenomOS

An educational x86-64 operating system, built from absolute scratch
in Assembly (boot stage) and C++ (kernel).

## Status
Phases complete: **1 (Bootloader), 2 (A20/GDT/Protected Mode → extended
to Long Mode), 3 (C++ Kernel/VGA), 4 (Memory Manager), 5 (Interrupts/
Keyboard/Shell), Ring 3 (User Mode), Multitasking (preemptive kernel
threads)**. Plus a bonus Snake game built on top of the interrupt/
keyboard/VGA stack.

## What each stage/component does

### Stage 1 (`boot/stage1.asm`)
BIOS loads this 512-byte boot sector to `0x7C00`. Sets up segments/
stack, prints a boot message, loads Stage 2 off disk, jumps to it
(passing the boot drive number forward in `DL`).

### Stage 2 (`boot/stage2.asm`)
1. Loads the kernel off disk into a temporary buffer.
2. Detects the real physical memory map via BIOS `int 15h, eax=0xE820`
   (`boot/mmap.asm`), stashing it in low memory for the kernel.
3. Enables A20, builds/loads a GDT (32-bit code, ring-0 data, 64-bit
   ring-0 code, **ring-3 code, ring-3 data, and a TSS descriptor** -
   see the Ring 3 section below).
4. Switches to Protected Mode, relocates the kernel to `0x100000`,
   builds minimal identity-mapped page tables (1GB, 2MB pages).
5. Enables PAE, Long Mode (EFER.LME), paging, far-jumps into true
   64-bit Long Mode.
6. Exports the TSS's address, its GDT descriptor's address, and the
   TSS selector to a fixed memory location for the kernel to read
   (see "Ring 3" below for why this hand-off exists).
7. Jumps to the kernel's entry point.

### Kernel (`kernel/`)
- `kernel_entry.asm`, `vga.cpp`/`.hpp`, `interrupts.*`, `keyboard.*`,
  `shell.*`, `snake.*` - see git history / earlier phases for details;
  unchanged in spirit through this phase, though `interrupts.asm`/
  `.hpp`/`.cpp` were refactored (see below).
- `pmm.cpp`/`.hpp` - Physical Memory Manager: bitmap over the first
  1GB, seeded from the real E820 map, VenomOS's own memory always
  explicitly reserved.
- `paging.cpp`/`.hpp` - dynamic `map_page()`/`unmap_page()`, walking
  and extending Stage 2's boot-time page tables, allocating new
  page-table frames from the PMM as needed. Includes `PAGE_USER` (see
  Ring 3 below) and a genuine `self_test()`.
- `heap.cpp`/`.hpp` - free-list `kmalloc()`/`kfree()` (first-fit,
  splitting, coalescing), growing on demand from the PMM.
- **`ring3.cpp`/`.hpp`** - brings up a TSS and performs a real
  Ring 0 → Ring 3 privilege transition (see below).
- **`user_program.asm`** - a standalone flat binary that runs at
  genuine CPL 3.
- **`user_program_blob.asm`** - embeds that binary into the kernel via
  `incbin`.
- `kernel.cpp`/`.hpp` - `kernel_main()`: interrupts, a real
  timer-measured pause so the genuine boot log is readable, memory
  management init, paging self-test, shell.

## Ring 3 (User Mode) — how it works

1. **GDT** (`boot/gdt.asm`) gained a ring-3 code segment (DPL=3), a
   ring-3 data segment (DPL=3), and a TSS descriptor. In 64-bit mode a
   TSS descriptor is 16 bytes (it needs a full 64-bit base address),
   unlike every other 8-byte descriptor here.
2. **TSS**: its only job in this design is holding `RSP0` - the stack
   pointer the CPU automatically switches to whenever an interrupt or
   exception arrives while running in ring 3. Without a valid TSS
   loaded via `LTR`, entering ring 3 at all would be unsafe (the very
   first timer tick or syscall from ring 3 would have nowhere
   trustworthy to build its stack frame). The TSS structure's base
   address and its descriptor's base-address fields can't be computed
   with NASM's bitwise operators against a label at assembly time in
   flat-binary mode (this was tried; it reliably errors), so both get
   **patched at runtime** by `kernel/ring3.cpp`, using real CPU
   shift/and instructions instead.
3. **`ring3::run_demo()`** sets up the TSS, maps a fresh page beyond
   the 1GB boundary with `PAGE_USER` set, copies `user_program.asm`'s
   machine code into it, maps a second page as its stack, then
   manually builds the 5-word `IRETQ` stack frame (SS, RSP, RFLAGS,
   CS, RIP with `CS`/`SS` selectors that have `RPL=3`) and executes
   `iretq` - a real, CPU-enforced privilege-level transition.
4. **`user_program.asm`** (genuinely running at CPL 3): calls a
   syscall (`int 0x80`, `rax`=1=`SYS_WRITE`) to print a message via
   the kernel, proving the syscall path works - then **deliberately
   executes `cli`**, a privileged instruction ring 3 cannot perform.
   The CPU itself raises `#GP` - this isn't simulated or asserted by
   VenomOS's own code.
5. **`interrupts.cpp`** recognizes a `#GP` whose saved `CS` has
   `RPL=3` as proof privilege separation is real, reports it, and
   halts (VenomOS has no process model yet to resume into - see
   "Honesty notes" below).

### Two real bugs found and fixed while building this
- **TSS selector**: the 104-byte TSS structure lives *inside* the
  GDT's address range (before its own descriptor), which pushes the
  descriptor's real byte offset well past the naively-assumed `0x30`.
  Fixed by having Stage 2 export the *actual computed* selector rather
  than the kernel assuming one.
- **Alignment**: an `align 16` before the TSS structure aligned its
  *absolute* memory address, not its *offset from `gdt_start`* - which
  is what actually needs to be a multiple of 8 for a selector to be
  valid. This left the TSS descriptor at a non-8-aligned offset, whose
  low bits then got misread as the Table Indicator bit (pointing at a
  nonexistent LDT) - the CPU faulted `LTR` itself. Fixed by removing
  the misguided alignment (unnecessary: 48 preceding bytes + 104-byte
  TSS are both already multiples of 8).
- A third, more subtle bug: `paging::self_test()` (run once at boot)
  and the ring 3 demo's user-code mapping landed in the *same*
  intermediate PML4/PDPT/PD table slots (their virtual addresses are
  only 4KB apart). Since `self_test()` ran first without `PAGE_USER`,
  and `paging::map_page()` only set flags on *newly created* tables,
  the *existing* intermediate entries never gained the USER bit -
  which x86 requires at *every* level of the translation hierarchy,
  not just the final page-table entry. Fixed by having
  `get_or_create_table()` widen (OR in) flags on existing entries too.

All three were caught by actually booting the change in QEMU and
reading real CPU state (`info registers`, `-d int` exception logs, raw
memory dumps) after each attempt - not assumed to work from code
review alone.

## Multitasking — how it works

Kernel-thread multitasking (every task shares the kernel's single
address space - see "scope" below), preemptively switched by the
timer interrupt.

1. **`kernel/task_switch.asm`**'s `switch_context(old_rsp_out, new_rsp)`
   is the one real trick: it saves only the *callee-saved* registers
   (`rbx`, `rbp`, `r12`-`r15`, `rflags` - everything else is already
   the caller's responsibility per the System V ABI, so there's
   nothing else to save), swaps `RSP`, restores the new task's own
   previously-saved registers, and executes a plain `ret`. That `ret`
   doesn't return to whoever *called* `switch_context()` - it resumes
   whatever task owns the *new* stack, exactly where that task was
   when it last called `switch_context()` itself (or, for a brand new
   task, at `task_trampoline` - see below). No interrupt frame, no
   `iretq`, just an ordinary `call`/`ret` pair doing something
   extraordinary.
2. **`kernel/task.cpp`** builds a *fake* initial stack for any new
   task, laid out exactly like what `switch_context()` expects to
   find (zeroed callee-saved registers, `rflags=0x202`, and a "return
   address" of `task_trampoline` instead of a real caller) - so
   switching to it for the first time starts the task running.
   `task_trampoline` calls the task's entry function, and if that
   function ever returns, cleanly tears the task down
   (`task::exit_current()`: mark `Terminated`, free its stack, ask the
   scheduler for something else to run).
3. **`kernel/scheduler.cpp`** is pure policy on top of that mechanism:
   every 5th timer tick (~275ms), round-robin to the next `Ready`
   task. `interrupts.cpp` calls `scheduler::tick()` on every `IRQ0`,
   *after* sending the PIC its EOI (important: a switch might not
   return control here again for a while, so the EOI can't wait).
4. **`kernel_main()`**'s own execution becomes "task 0" (`task::init()`)
   - the shell isn't special-cased, it's just the first task, which
   is what lets the scheduler preempt *it* too and still have
   something to switch back to.
5. **Ring 3 integration**: the `usermode` shell command now spawns
   `ring3::run_demo` as its own task (`task::create(&ring3::run_demo,
   ...)`) instead of calling it inline. When the demo's deliberate
   `#GP` fires, `interrupts.cpp`'s handler calls `task::exit_current()`
   instead of halting forever - cleanly killing *that* task and
   switching back to whatever else is `Ready` (the shell). This is
   exactly the capability the Ring 3 section's original "no process
   model exists yet" note said was missing - confirmed working by
   spawning `usermode`, then immediately queuing a `help` keystroke,
   and watching `help`'s actual output appear *after* the fault
   message, from the shell task resuming normally.

### Scope, stated honestly
- **Single shared address space**: every task uses the same `CR3` -
  this is kernel-thread multitasking, not process isolation. A bug in
  one task's pointer arithmetic can still corrupt another task's
  memory. Separate per-task address spaces (a real `CR3` per task) is
  the natural next step, not something silently glossed over.
- **Single shared `TSS.RSP0`**: every ring 3 → ring 0 transition, from
  any task, currently lands on the same fixed kernel stack. Fine for
  one ring 3 excursion at a time (this demo); would need per-task
  `RSP0` management before multiple tasks could safely run ring 3 code
  concurrently.
- No task priorities, no sleep/wake, no inter-task synchronization
  primitives (mutexes/semaphores) yet - just round-robin and
  cooperative `yield()`.

## Folder structure
```
VenomOS/
├── boot/
│   ├── stage1.asm, stage2.asm, disk.asm, print.asm, a20.asm,
│   │   gdt.asm, mmap.asm
├── kernel/
│   ├── kernel_entry.asm, kernel.cpp/.hpp, vga.cpp/.hpp,
│   │   interrupts.asm/.cpp/.hpp, keyboard.cpp/.hpp, shell.cpp/.hpp,
│   │   snake.cpp/.hpp, pmm.cpp/.hpp, paging.cpp/.hpp, heap.cpp/.hpp,
│   │   ring3.cpp/.hpp, user_program.asm, user_program_blob.asm,
│   │   task.cpp/.hpp, scheduler.cpp/.hpp, task_switch.asm,
│   │   linker.ld
├── include/
│   ├── io.hpp, stdint.hpp, stddef.hpp
├── build/        (generated, not committed)
├── Makefile
└── README.md
```

## Building (Windows host)
Install a real **x86_64-elf** cross-compiler and put its `bin/` folder
on your `PATH`, ahead of any MSYS2/MinGW `bin/` folder. Also install
NASM, `make`, and QEMU.

```
make clean
make all
make run
```

## Shell commands
`help`, `clear`, `version`, `whoami`, `snake`, `meminfo`, `memtest`,
**`tasks`** (spawns two preemptively-scheduled counter tasks on rows
20/21 - the shell stays fully usable while they run), **`usermode`**
(spawns the Ring 3 demo as its own task - the shell keeps running
immediately, and resumes normally after the demo's task is terminated
by its deliberate fault).

## Honesty notes (kept here on purpose)
- `kernel_main()` waits using the real hardware timer while the
  genuine boot log (from actual Real Mode/Protected Mode/Long Mode
  execution) stays on screen, rather than clearing it and reprinting a
  fabricated copy.
- `paging::self_test()` and the `memtest` shell command are real,
  functional tests (allocate, write a pattern, read it back, compare)
  - not hardcoded "PASS" strings.
- `handle_syscall()`'s `SYS_WRITE` dereferences the ring-3-supplied
  pointer directly, without the "copy_from_user" validation a real
  kernel would use to stop user code tricking the kernel into reading
  arbitrary memory. Documented in `interrupts.cpp` as a deliberately
  scoped simplification (the only ring 3 code that exists is
  VenomOS's own demo program), not silently skipped.
- Multitasking is kernel-thread-only (shared address space) with a
  single shared `TSS.RSP0` - see "Scope, stated honestly" under
  Multitasking above.

## Verified (this session)
- Full clean build with zero errors.
- E820 detection, PMM stats, paging self-test, heap alloc/free, the
  full Ring 3 transition, and multitasking were all exercised via
  actual simulated keystrokes (QEMU monitor `sendkey`) and confirmed
  by reading the VGA text buffer back - not just assumed from source
  inspection.
- The Ring 3 demo's success was independently cross-checked via CPU
  exception logging (`-d int`) and raw physical-memory dumps of the
  TSS/GDT bytes while debugging three real bugs found along the way
  (see the Ring 3 section above) - not just the on-screen messages.
- **`tasks`**: spawned two counter tasks, confirmed both counters
  advancing with different values at a snapshot mid-run (proving real
  interleaving, not sequential execution), confirmed `help` executes
  correctly *while they're still running* (shell responsiveness under
  preemption), and confirmed both cleanly print `(done)` and terminate
  after their loop.
- **`usermode` + multitasking integration**: spawned the Ring 3 demo
  as a task, queued a `help` keystroke immediately after, and watched
  `help`'s actual command output appear *after* the fault-handling
  messages - direct proof the shell task resumed normally post-fault,
  fulfilling the promise made in the original Ring 3 section.
- `memtest`, `meminfo`, and `snake` regression-tested after all
  multitasking changes to confirm nothing else broke.

## Next phase ideas (not started)
Per-task address spaces (real process isolation, not just kernel
threads), a simple filesystem, hardening the syscall path with real
pointer validation, or synchronization primitives (mutex/semaphore)
now that concurrent tasks genuinely exist.
