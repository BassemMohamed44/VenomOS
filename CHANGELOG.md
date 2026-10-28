# Changelog

All notable changes to VenomOS will be documented here.

---

## [0.6.0] - 2026

### Added

- Physical Memory Manager (bitmap, driven by the BIOS E820 map)
- Paging with page protection, NX, and per-process address spaces
- Kernel heap (`kmalloc` / `kfree`)
- Preemptive round-robin scheduler, process control blocks, process
  lifecycle (PID, parent/child, states, exit codes, `wait`, sleep)
- Ring 3 user mode, TSS, `int 0x80` syscalls (`exit`, `write`, `sleep`),
  validated user-pointer access (`usercopy`)
- ATA PIO driver and VenomFS filesystem
- ELF64 loader with hardened validation (`W^X`, bounds and overlap checks)
- Shell commands: `meminfo`, `memtest`, `tasks`, `usermode`,
  `stackoverflow`, `ls`, `cat`, `write`, `rm`, `run`, `ps`, `wait`, `sleep`
- Boot-time self-tests

### Changed

- README, `docs/ARCHITECTURE.md` and `docs/ROADMAP.md` rewritten to match the
  implemented architecture, phase status, limitations and open work
- `docs/STEP-BY-STEP.md` now marks statements that later phases superseded
- The boot banner and the `version` command now report `0.6.0`. The version
  string lives in one place, `include/version.hpp` (it was a hardcoded
  `0.2.0` in two files)
- The "file too large" message from `run` no longer says "in this phase"

---

## [0.5.0] - 2026

### Added

- Interactive shell
- Snake game
- Keyboard driver
- VGA text rendering

---

## [0.4.0]

### Added

- Interrupt Descriptor Table
- Keyboard IRQ
- Interrupt handlers

---

## [0.3.0]

### Added

- Long Mode support
- 64-bit kernel
- Linker script
- C++ kernel entry

---

## [0.2.0]

### Added

- Protected Mode
- Global Descriptor Table
- Stage 2 Bootloader

---

## [0.1.0]

### Added

- Stage 1 Bootloader
- BIOS disk loading
- Bootable disk image