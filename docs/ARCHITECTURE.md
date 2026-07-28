# VenomOS Architecture

## Boot Flow

```
BIOS
    │
    ▼
Stage 1
    │
    ▼
Stage 2
    │
    ▼
Protected Mode
    │
    ▼
Long Mode
    │
    ▼
Kernel
    │
    ├── VGA Driver
    ├── Keyboard Driver
    ├── Interrupt Manager
    ├── Shell
    └── Snake Game
```

## Components

- Bootloader
- Kernel
- Interrupts
- Drivers
- Shell
- Applications