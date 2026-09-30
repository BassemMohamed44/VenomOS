# VenomOS

An educational x86-64 operating system, built from absolute scratch
in Assembly (boot stage) and C++ (kernel).

## Status
Phases complete: **1 (Bootloader), 2 (A20/GDT/Protected Mode → extended
to Long Mode), 3 (C++ Kernel/VGA), 4 (Memory Manager), 5 (Interrupts/
Keyboard/Shell), Ring 3 (User Mode), Multitasking (preemptive kernel
threads), Filesystem (VenomFS), Process Loading (isolated address
spaces), ELF Loader (real ELF64 parsing, multi-segment), Phase A
(hardened ELF validation), Phase B (full process lifecycle: PCB, PID/
parent/child, real state machine, exit codes)**. Plus a bonus Snake
game built on top of the interrupt/keyboard/VGA stack.

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

## Filesystem (VenomFS) — how it works

1. **Boot medium switch**: Booting from BIOS `int 13h` (Real Mode
   only) worked fine on a 1.44MB floppy, but Long Mode has no BIOS to
   ask for disk access anymore, and a floppy has no room for a real
   filesystem anyway. The disk image grew to 16MB and is now booted
   as an IDE hard disk (`if=ide -boot c` instead of `if=floppy
   -boot a`) - verified this needed **zero changes** to
   `boot/stage1.asm`/`stage2.asm`: QEMU's SeaBIOS answers CHS-style
   `int 13h` reads identically for its emulated IDE disk, and the
   boot drive number in `DL` just becomes `0x80` instead of `0x00`,
   which the existing code already handled generically.
2. **`kernel/ata.cpp`/`.hpp`**: a polling-mode (no IRQ needed) ATA PIO
   driver for the primary channel's master drive - the only way the
   kernel can read/write sectors once Long Mode is active. `init()`
   issues `IDENTIFY` as a real presence/sanity check, not just an
   assumption that a drive exists.
3. **`kernel/fs.cpp`/`.hpp`** ("VenomFS"): a deliberately simple,
   custom filesystem - not FAT/ext/anything standards-based, chosen so
   it's small enough to fully verify end-to-end rather than because
   it's how a production filesystem would be built. Flat namespace (no
   directories), one contiguous run of sectors per file (no
   fragmentation-chain bookkeeping), a free bitmap for reuse after
   delete:
   - **Superblock** (LBA 256) - magic `"VNFS"`, layout info.
   - **File table** (LBA 257, 6 sectors) - 64 fixed-size entries
     (name, size, start LBA, block count, used flag).
   - **Free bitmap** (LBA 263, 8 sectors) - one bit per data block.
   - **Data region** (LBA 271 onward) - file contents.
   - `fs::init()` checks the superblock's magic; if it doesn't match
     (a brand-new disk image), it **self-formats** automatically -
     no separate host-side `mkfs` tool needed.
4. **Shell commands**: `ls`, `cat <file>`, `write <file> <text>`
   (creates or fully overwrites), `rm <file>`.

### Verified, including the test that matters most for a filesystem
Every operation was exercised via actual simulated keystrokes and
confirmed by reading the screen back - `write` followed by `ls`
showing the right size, `cat` reading back byte-for-byte what was
written, `rm` removing a file and `cat`/`ls` afterward correctly
reporting it gone, and attempting to `rm`/`cat` a name that was never
created correctly reporting "File not found" rather than silently
succeeding or reading garbage.

**Persistence was verified across a full reboot**, not just within one
boot session (which alone wouldn't rule out an in-memory-only fake): a
file was created, the QEMU VM was fully quit and restarted from the
*same* disk image (not rebuilt/reformatted), and `ls`/`cat` after the
fresh boot showed the file with its exact original content still
there - proof this genuinely persists to disk rather than just living
in a RAM cache that happened to survive within one run.

## Process Loading & ELF — the convergence point

Everything from Filesystem, Ring 3, Multitasking, and Paging comes
together here: `run <file>` loads a flat binary **from VenomFS** and
runs it as a genuinely isolated process - not a kernel-shared-address-
space thread like the `tasks` demo, and not an embedded-in-the-kernel
binary like the original `usermode` demo.

1. **`kernel/paging.cpp` gained per-address-space operations**:
   - `map_page_in(pml4, ...)` - the same table-walking logic as
     `map_page()`, but against an explicitly given PML4 instead of
     always "whatever CR3 is right now" (`map_page()` is now just
     `map_page_in()` using the current CR3).
   - `create_address_space()` - allocates a fresh PML4 and PDPT, whose
     slot 0 **borrows** (shares the same physical PD, not a copy) the
     kernel's own boot-time 1GB identity map, so kernel code/data/
     interrupt handlers/heap stay reachable no matter whose address
     space is loaded - every other PDPT slot starts empty, ready for
     process-private mappings.
   - `destroy_address_space()` - frees every frame private to a given
     address space (walking PDPT slots 1+ down through PD/PT/data
     frames) without ever touching the shared kernel PD in slot 0.
2. **`kernel/task.cpp` gained a `cr3` field**: `create()` (kernel
   threads - the shell, the counter demo tasks) leaves it 0, meaning
   "share the kernel's own address space". `create_process()` sets it
   to a real, isolated PML4. `switch_to()` only issues `mov cr3`
   (which flushes the whole TLB) when the incoming task's target
   address space actually differs from what's physically loaded.
   `exit_current()` calls `destroy_address_space()` for any task that
   had a private one, so process memory doesn't leak on exit.
3. **`kernel/ring3.cpp` was refactored**: `setup_tss()` and the new
   `enter(entry_virt, stack_top)` are now reusable primitives, used by
   both the original embedded `usermode` demo and the new process
   loader - instead of duplicating the TSS/IRETQ logic in two places.
4. **`kernel/elf.hpp`/`.cpp`**: real ELF64 structures (`Elf64_Ehdr`,
   `Elf64_Phdr`) and `is_valid_executable()`, which checks the magic
   bytes, 64-bit/little-endian class, `ET_EXEC` type, and
   `EM_X86_64` machine - rejecting anything that isn't a genuine,
   statically-linked x86-64 executable rather than assuming.
5. **`kernel/process.cpp`**: `run(filename)` reads the file from
   VenomFS, validates its ELF header, then walks **every** `PT_LOAD`
   program header (not just one hardcoded segment) - for each one,
   allocates the pages it needs, copies exactly `p_filesz` bytes from
   the right file offset (zero-filling the rest, which is how `.bss`
   works without taking any space in the file), and maps each page
   into the process's isolated address space with permissions
   translated from the segment's `p_flags`. The process's entry point
   is the ELF header's real `e_entry` - not a fixed constant - stashed
   on its `Task` (`task.cpp` gained `user_entry`/`user_stack_top`
   fields) so `process_entry()` can hand off to `ring3::enter()`
   correctly regardless of what the ELF file actually specified.
6. **`kernel/demo_elf.asm`**: unlike `user_program.asm` (a flat
   `-f bin` binary with a hardcoded `ORG`), this is assembled as a
   relocatable object (`-f elf64`) and linked with the new
   **`kernel/user_link.ld`** into a genuine `ET_EXEC` ELF64
   executable, based at `0x40000000` - real `readelf -l` output shows
   two separate `PT_LOAD` segments (`.text`, R+E; `.rodata`, R-only),
   confirming this is a real multi-segment executable, not a
   single-blob binary wearing an ELF header. Prints a message via
   `SYS_WRITE`, then calls the `SYS_EXIT` syscall
   (`interrupts.cpp` → `task::exit_current()`) for a clean, voluntary
   termination - unlike `user_program.asm`'s deliberate crash. Written
   to VenomFS as `hello.elf` automatically at every boot
   (`kernel.cpp`), since there's no external tool in this environment
   to place a binary onto the disk image from outside the kernel.

### A real bug this uncovered: the 1GB boundary
The very first version of `user_link.ld` based the demo executable at
the traditional `0x400000` (4MB) - and every `PT_LOAD` segment failed
to map, because every process's address space **shares** (borrows)
the kernel's boot-time identity map for the whole first 1GB (built out
of 2MB huge pages), and `paging::map_page_in()` correctly *refuses* to
map a 4KB page inside a range a huge page already covers rather than
silently corrupting it. Fixed by basing user executables at `0x40000000`
(1GB) instead - solidly inside each process's own, private address
range. A genuine example of the kind of bug only running the actual
code in QEMU catches, not something visible from reading the linker
script alone.

Scope limit, documented rather than hidden: no dynamic linking/PIE
(`ET_EXEC` only), no NX enforcement (an executable and a non-executable
segment are mapped identically permission-wise, since `paging.cpp`
doesn't implement the no-execute bit yet), and a 64KB cap on the ELF
file itself.

## Phase A — Hardened ELF validation

Requested explicitly, before adding any more loader features: every
item on this list is a real, separately-checked failure mode in
`elf::validate()` (`kernel/elf.cpp`), not a single generic "looks
okay" pass.

- **`p_memsz >= p_filesz`** - rejected explicitly; a segment can't
  claim to occupy less memory than the file data being copied into it.
- **Overflow checks** - every offset/size addition that feeds into a
  bounds comparison (`p_offset+p_filesz`, `p_vaddr+p_memsz`,
  `e_phoff+`the program header table's total size) is checked with an
  `add_overflows()` helper *before* the addition, not after - an
  attacker-controlled file with, say, `p_vaddr` near `UINT64_MAX`
  can't wrap a bounds check around to "pass".
- **`p_align` validated** - must be `0` or a genuine power of two, and
  (per the ELF spec) `p_vaddr` and `p_offset` must agree modulo
  `p_align`.
- **Entry point validated** - `e_entry` must land inside the virtual
  address range of some segment that's actually being loaded, or the
  CPU would jump straight into unmapped memory the instant the
  process starts.
- **User virtual-address range validated** - every segment (and the
  entry point) must fall entirely within `[0x40000000, 2^47)` -
  VenomOS's user-space window (see "prevent kernel-space mappings"
  below for why the lower bound matters, and canonical-address rules
  for the upper one).
- **Overlapping segments detected** - every `PT_LOAD` segment's
  page-aligned range is checked against every previously-validated
  one; any overlap is rejected.
- **Kernel-space mappings prevented** - the same lower bound as
  above: every process address space *shares* the kernel's boot-time
  identity map for the first 1GB (see `paging::create_address_space()`),
  so a segment placed there would either silently alias kernel memory
  or (correctly, thanks to `paging::map_page_in()`'s own huge-page
  check) simply fail to map - `elf::validate()` catches it earlier,
  with a clear reason, before any mapping is even attempted.
- **Unsupported types/flags rejected** - `e_type` must be `ET_EXEC`
  (no PIE/shared objects), `e_machine` must be `EM_X86_64`, a
  `PT_LOAD` segment's `p_flags` must be non-zero and contain no bits
  outside `PF_READ|PF_WRITE|PF_EXEC`, and **`p_type` uses an
  allowlist**: `PT_LOAD`, `PT_NULL` (spec-defined "ignore this"), and
  `PT_GNU_STACK` (VenomOS's own toolchain always emits one - confirmed
  via `readelf -l` on the built demo) are tolerated; anything else
  (`PT_DYNAMIC`, `PT_INTERP`, `PT_NOTE`, ...) is explicitly
  **rejected**, not silently skipped.
- **Cleanup on failure** - restructured so validation happens
  *entirely* before `paging::create_address_space()` is ever called:
  a malformed file is rejected with zero allocations made and nothing
  to clean up. Once validation passes, every subsequent failure path
  (PMM out of frames, a mapping call failing) calls
  `paging::destroy_address_space()` before returning - **and** (see
  "Three follow-up fixes" below) any individual frame that was
  allocated but never successfully mapped is freed explicitly first,
  since `destroy_address_space()` can only find frames that actually
  made it into the page tables.
- **Tested against malformed files** - `process::self_test()`
  (`kernel/process.cpp`) builds **16** deliberately malformed variants
  of the real embedded demo executable (bad magic, truncated below the
  header size, wrong `e_type`/`e_machine`, `p_memsz < p_filesz`, a
  kernel-space `p_vaddr`, a non-power-of-two `p_align`, zero flags,
  unrecognized flag bits, segment data running past EOF, integer
  overflow in `p_offset+p_filesz`, an alignment *mismatch* between
  `p_vaddr` and `p_offset` with a valid power-of-two align, a segment
  above `USER_SPACE_MAX`, an unsupported `p_type`, an out-of-segment
  entry point, two overlapping segments) plus the unmodified original,
  and confirms every single one is accepted or rejected exactly as
  expected. Runs automatically at boot
  ("`ELF loader self-test: PASS`"), the same pattern as
  `paging::self_test()`.

### Three follow-up fixes (a second hardening pass)
- 🔴 **Allocation leak fixed** - in both `load_segments()` and the
  stack-mapping code in `process::run()`, if `pmm::alloc_frame()`
  succeeded but the immediately-following `paging::map_page_in()`
  call then failed (the realistic case: a huge-page collision
  `paging.cpp` correctly refuses), the frame was never freed. Since
  it never made it into any page table, `paging::destroy_address_space()`
  has no way to discover and free it on its own - both call sites now
  explicitly `pmm::free_frame()` it before returning.
- 🟠 **`p_type` handling made explicit** - previously any `p_type !=
  PT_LOAD` was silently skipped (fine for `PT_GNU_STACK`, but would
  have silently ignored, say, a `PT_DYNAMIC` segment instead of
  refusing to run a file this loader can't actually satisfy). Now an
  explicit allowlist (`PT_LOAD`/`PT_NULL`/`PT_GNU_STACK`) - anything
  else is a hard rejection with a clear reason.
- 🟡 **Self-test coverage extended** - added the four edge cases that
  were missing: integer overflow, an alignment *mismatch* (as opposed
  to an invalid alignment value), a segment above `USER_SPACE_MAX`,
  and an unsupported segment type - bringing the suite from 12 cases
  to 16.

### Verified interactively too
Wrote a plain-text file, named it `notreal.elf`, and ran it - the
loader correctly reported `"Rejected: File too small to contain an
ELF header."` (39 bytes, below `sizeof(Elf64_Ehdr)` = 64) instead of
crashing or silently doing nothing, and the shell immediately went on
to run the real `hello.elf` correctly right after - proof a rejected
file doesn't leave anything in a bad state. Also ran `hello.elf`
several times in a row and watched `meminfo`: physical frame usage
only grew by the expected one-time cost of `ring3::setup_tss()`'s
persistent RSP0 stack (a documented, intentional allocation made once
ever, not a leak) - each individual successful run's own frames were
correctly reclaimed on exit.

## Phase B — Process Lifecycle

`task::Task` (`kernel/task.hpp`/`.cpp`) is now a genuine PCB, not just a
scheduler entry: a PID, a parent PID, a real 7-state machine, and an
exit code. Every state is actually reachable and exercised, not just
declared:

```
NEW -> READY -> RUNNING -> { BLOCKED | SLEEPING } -> READY -> ...
                    |
                    v
              ZOMBIE -> DEAD   (or straight to DEAD if orphaned)
```

- **PID / Parent / Children** - `create()`/`create_process()`
  auto-assign `parent_pid = task::current()->pid` (whichever task
  called them). "Children" isn't a stored list - it's a query:
  `wait_for_child()` and the shell's `ps` command both scan the task
  table for `parent_pid == X`, keeping the PCB itself small.
- **BLOCKED** - `task::block_current()`/`unblock(pid)` are the
  general primitives; the concrete use case wiring them together is
  `wait_for_child()` (below).
- **SLEEPING** - `task::sleep_current(ticks)` sets a `wake_tick` and
  blocks; `scheduler::tick()` now scans every timer interrupt (not
  just its round-robin switch boundary) for any Sleeping task whose
  time has come and promotes it back to Ready. Exposed as `SYS_SLEEP`
  for ring 3 code, and directly as the shell's `sleep <ticks>` command
  for testing.
- **ZOMBIE / exit codes / reaping** - `SYS_EXIT` now takes a real exit
  code (`rdi`). `exit_current(exit_code)` frees the task's stack and
  (if it had one) its private address space, then - if it still has a
  *live* parent - becomes a **Zombie** (PCB kept, resources gone) and
  wakes that parent if it's Blocked in `wait_for_child()`. An orphan
  (no parent, or the parent's already gone) skips Zombie entirely and
  goes straight to **Dead**, since nothing could ever reap it.
  `wait_for_child()` scans for a Zombie child, captures its PID/exit
  code, marks it Dead (slot freed for reuse), and returns - or blocks
  first if no Zombie child exists yet but the caller does have at
  least one child. The ring 3 fault path (`usermode`'s deliberate
  crash) now calls `exit_current(-1)`, a sentinel distinct from any
  real program's own chosen code.
- **Idle task** - `task::init()` now also creates a permanent,
  always-Ready `idle` task (`sti; hlt` in a loop) - the safe fallback
  the scheduler switches to whenever something blocks/sleeps/exits
  and nothing else happens to be Ready.
- **Shell commands**: `ps` (lists every live PCB: PID, PPID, state,
  name, and exit code for zombies), `wait` (blocks until any child of
  the shell exits, reaping it), `sleep <ticks>`.

### A real bug found while verifying this
The idle task, created from inside `task::init()` while the shell is
`current()`, would otherwise inherit `parent_pid = 1` (the shell) -
making `wait` see it as an eternal "child" that never exits, and
**block forever** the very first time `wait` was called with nothing
actually spawned yet. Caught by literally running `wait` with no
children and watching the shell hang instead of returning immediately.
Fixed by explicitly clearing idle's `parent_pid` to `NO_PID` right
after creating it - it's a system task, not part of the process tree.

### Verified end-to-end
```
ps                    -> PID 1 shell (RUNNING), PID 2 idle (READY, PPID 0)
run hello.elf          -> spawns PID 3, parented to the shell (PID 1)
ps                     -> PID 3 now ZOMBIE (exit code 42) - the exact
                           code kernel/demo_elf.asm passed to SYS_EXIT
wait                   -> "PID 3 exited with code 42"
ps                     -> PID 3 is gone entirely (reaped -> Dead)
wait (nothing spawned) -> returns immediately: "No child processes..."
sleep 36               -> shell pauses for ~2 seconds, then "Awake."
                          on its own, no input needed
```
`memtest`, `usermode` (now exercising the `-1` fault exit code path),
and `whoami` regression-tested again afterward to confirm nothing else
broke.

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
│   │   ata.cpp/.hpp, fs.cpp/.hpp,
│   │   process.cpp/.hpp, elf.cpp/.hpp, demo_elf.asm,
│   │   demo_hello_blob.asm, user_link.ld,
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
Boots a 16MB disk image as an IDE hard disk (not a floppy). The first
boot ever on a fresh image auto-formats VenomFS; later boots (as long
as you don't `make clean` the image away) see the same disk and keep
whatever files you've created.

## Shell commands
`help`, `clear`, `version`, `whoami`, `snake`, `meminfo`, `memtest`,
`tasks` (spawns two preemptively-scheduled counter tasks on rows
20/21 - the shell stays fully usable while they run), `usermode`
(spawns the Ring 3 demo as its own task - the shell keeps running
immediately, and resumes normally after the demo's task is terminated
by its deliberate fault), `ls`, `cat <file>`, `write <file> <text>`,
`rm <file>`, `run <file>` (parses a real ELF64 executable from
VenomFS, maps every `PT_LOAD` segment into its own isolated address
space, and runs it as a genuine process - try `run hello.elf`),
**`ps`** (lists every live PCB: PID, PPID, state, name, exit code for
zombies), **`wait`** (blocks until any child of the shell exits,
reaping it and reporting its exit code), **`sleep <ticks>`** (puts the
shell task itself to sleep for real, ~18 ticks/second).

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
- **VenomFS**: confirmed the boot-medium switch (floppy → 16MB IDE
  disk) needed no bootloader changes; confirmed `write`/`ls`/`cat`/`rm`
  all work correctly including "File not found" for nonexistent names;
  confirmed data **survives a full VM restart from the same disk
  image**, the strongest evidence a filesystem is genuinely
  disk-backed rather than an in-memory illusion.
- `memtest`, `meminfo`, `usermode`, and `snake` regression-tested again
  after the filesystem work to confirm nothing else broke.
- **Process Loading & ELF Loader**: confirmed via `readelf -h`/`-l` on
  the standalone-built demo executable that it's a genuine `ET_EXEC`
  ELF64 file with two separate `PT_LOAD` segments before ever
  embedding it in the kernel. `ls` confirmed `hello.elf` (seeded at
  boot) is really on VenomFS. `run hello.elf` showed the shell prompt
  return *immediately* (non-blocking, same pattern as `usermode`),
  then the process's own message - "Hello from a genuine ELF64
  executable - parsed, loaded, and run by VenomOS's own ELF loader!" -
  appeared via a real `SYS_WRITE` syscall from inside its isolated
  address space, followed by a clean `SYS_EXIT`. **A real bug was
  found and fixed along the way**: the first attempt based the demo
  executable at the traditional `0x400000`, which collides with the
  kernel's shared low-1GB identity map (huge pages) every process
  address space borrows - `paging::map_page_in()` correctly refused
  rather than corrupting anything, `run` reported "Failed to map a
  segment" honestly, and the fix (basing user executables at `0x40000000`
  instead) was verified by rerunning in QEMU, not just reasoned about.
  `whoami`, `memtest`, and `usermode` regression-tested again
  afterward to confirm nothing else broke.
- **Phase A (hardened validation)**: `process::self_test()`'s 16
  malformed-file cases plus the valid baseline all confirmed at boot
  (`ELF loader self-test: PASS`). Additionally verified interactively:
  wrote a plain-text file named `notreal.elf` and ran it - correctly
  rejected (`"File too small to contain an ELF header."`) rather than
  crashing, and the shell immediately ran the real `hello.elf`
  correctly right afterward, proving a rejection leaves nothing in a
  bad state. `whoami`, `memtest`, `usermode`, and `meminfo`
  regression-tested again after this work to confirm nothing else broke.
- **Phase B (process lifecycle)**: the full `ps`/`run`/`wait`/`ps`
  sequence above confirmed PID assignment, parent linkage, the
  RUNNING/READY/ZOMBIE states, and a real exit code (42) surviving the
  whole `SYS_EXIT` → Zombie → `wait_for_child()` → Dead pipeline
  intact. `sleep <ticks>` confirmed the shell task genuinely pauses
  and resumes on its own via the scheduler's per-tick wake check, with
  no input needed. A real bug (idle task wrongly parented to the
  shell, causing `wait` to block forever with nothing actually
  spawned) was caught by running exactly that scenario, not just by
  code review - fixed and reverified. `memtest`, `usermode` (now
  exercising the `-1` fault exit code), and `whoami` regression-tested
  again afterward.

## Next phase ideas (not started)
Ring-3 processes spawning their own children (currently only the
shell, in ring 0, can call `process::run()`), waiting for a *specific*
PID instead of just "any child", dynamic linking/PIE support, NX
(no-execute) enforcement now that ELF segment permissions are parsed
but not fully applied, hardening the syscall path with real pointer
validation, synchronization primitives (mutex/semaphore) now that
concurrent isolated processes genuinely exist, or growing VenomFS
toward directories and non-contiguous (chained) block allocation.
