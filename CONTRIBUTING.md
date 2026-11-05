# Contributing to VenomOS

Thank you for your interest in contributing!

## Getting Started

Clone the repository

```bash
git clone https://github.com/BassemMohamed44/VenomOS.git
```

Build

```bash
make
```

Clean rebuild, and a check before every commit that no generated file is tracked (`build/` is git-ignored and must never be committed):

```bash
make clean && make
make check-clean
```

Run

```bash
make run
```

---

## Coding Style

- Use meaningful names.
- Keep functions small.
- Comment low-level Assembly.
- Follow existing project structure.
- Use LF line endings.

---

## Pull Requests

Before opening a PR:

- Project builds successfully.
- No compiler warnings.
- Code is documented.
- README updated if necessary.

---

## Bug Reports

Please include:

- OS version
- QEMU version
- Steps to reproduce
- Expected behavior
- Actual behavior
