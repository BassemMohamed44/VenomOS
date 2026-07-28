# ============================================================
# VenomOS Makefile - (64-bit revision)
# ============================================================

ASM        := nasm
CXX        := x86_64-elf-g++
LD         := x86_64-elf-ld
OBJCOPY    := x86_64-elf-objcopy

BUILD_DIR  := build
BOOT_DIR   := boot
KERNEL_DIR := kernel
INCLUDE_DIR:= include

STAGE1_SRC := $(BOOT_DIR)/stage1.asm
STAGE2_SRC := $(BOOT_DIR)/stage2.asm

STAGE1_BIN := $(BUILD_DIR)/stage1.bin
STAGE2_BIN := $(BUILD_DIR)/stage2.bin
IMAGE      := $(BUILD_DIR)/venomos.img

QEMU       := qemu-system-x86_64

CXXFLAGS := -ffreestanding -fno-exceptions -fno-rtti \
            -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -nostdlib -Wall -Wextra -std=c++17 -Wa,--noexecstack \
            -I$(INCLUDE_DIR) -I$(KERNEL_DIR)

KERNEL_SECTOR_COUNT := 64
KERNEL_BUDGET_BYTES := $(shell echo $$(( $(KERNEL_SECTOR_COUNT) * 512 )))

KERNEL_OBJS := \
	$(BUILD_DIR)/kernel_entry.o \
	$(BUILD_DIR)/kernel.o \
	$(BUILD_DIR)/vga.o \
	$(BUILD_DIR)/keyboard.o \
	$(BUILD_DIR)/interrupts.o \
	$(BUILD_DIR)/interrupts_asm.o \
	$(BUILD_DIR)/shell.o \
	$(BUILD_DIR)/snake.o

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_RAW := $(BUILD_DIR)/kernel_raw.bin
KERNEL_BIN := $(BUILD_DIR)/kernel.bin

.PHONY: all clean run run-headless

all: $(IMAGE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(STAGE1_BIN): $(STAGE1_SRC) $(BOOT_DIR)/print.asm $(BOOT_DIR)/disk.asm | $(BUILD_DIR)
	$(ASM) -f bin -I $(BOOT_DIR)/ $(STAGE1_SRC) -o $(STAGE1_BIN)
	@size=$$(stat -c%s $(STAGE1_BIN)); \
	if [ "$$size" -ne 512 ]; then \
		echo "ERROR: stage1.bin is $$size bytes"; \
		exit 1; \
	fi
	@echo "[OK] stage1.bin is exactly 512 bytes"

$(STAGE2_BIN): $(STAGE2_SRC) $(BOOT_DIR)/print.asm $(BOOT_DIR)/a20.asm $(BOOT_DIR)/gdt.asm $(BOOT_DIR)/disk.asm | $(BUILD_DIR)
	$(ASM) -f bin -I $(BOOT_DIR)/ $(STAGE2_SRC) -o $(STAGE2_BIN)
	@size=$$(stat -c%s $(STAGE2_BIN)); \
	if [ "$$size" -ne 3072 ]; then \
		echo "ERROR: stage2.bin is $$size bytes"; \
		exit 1; \
	fi
	@echo "[OK] stage2.bin is exactly 3072 bytes"

$(BUILD_DIR)/kernel_entry.o: $(KERNEL_DIR)/kernel_entry.asm | $(BUILD_DIR)
	$(ASM) -f elf64 $(KERNEL_DIR)/kernel_entry.asm -o $(BUILD_DIR)/kernel_entry.o

$(BUILD_DIR)/kernel.o: $(KERNEL_DIR)/kernel.cpp $(KERNEL_DIR)/kernel.hpp $(KERNEL_DIR)/vga.hpp $(KERNEL_DIR)/keyboard.hpp $(KERNEL_DIR)/interrupts.hpp $(KERNEL_DIR)/shell.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/kernel.cpp -o $(BUILD_DIR)/kernel.o

$(BUILD_DIR)/vga.o: $(KERNEL_DIR)/vga.cpp $(KERNEL_DIR)/vga.hpp $(INCLUDE_DIR)/io.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/vga.cpp -o $(BUILD_DIR)/vga.o

$(BUILD_DIR)/keyboard.o: $(KERNEL_DIR)/keyboard.cpp $(KERNEL_DIR)/keyboard.hpp $(INCLUDE_DIR)/io.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/keyboard.cpp -o $(BUILD_DIR)/keyboard.o

$(BUILD_DIR)/interrupts.o: $(KERNEL_DIR)/interrupts.cpp $(KERNEL_DIR)/interrupts.hpp $(KERNEL_DIR)/keyboard.hpp $(INCLUDE_DIR)/io.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/interrupts.cpp -o $(BUILD_DIR)/interrupts.o

$(BUILD_DIR)/interrupts_asm.o: $(KERNEL_DIR)/interrupts.asm | $(BUILD_DIR)
	$(ASM) -f elf64 $(KERNEL_DIR)/interrupts.asm -o $(BUILD_DIR)/interrupts_asm.o

$(BUILD_DIR)/shell.o: $(KERNEL_DIR)/shell.cpp $(KERNEL_DIR)/shell.hpp $(KERNEL_DIR)/snake.hpp $(KERNEL_DIR)/vga.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/shell.cpp -o $(BUILD_DIR)/shell.o

$(BUILD_DIR)/snake.o: $(KERNEL_DIR)/snake.cpp $(KERNEL_DIR)/snake.hpp $(KERNEL_DIR)/keyboard.hpp $(KERNEL_DIR)/vga.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(KERNEL_DIR)/snake.cpp -o $(BUILD_DIR)/snake.o

$(KERNEL_ELF): $(KERNEL_OBJS) $(KERNEL_DIR)/linker.ld
	$(LD) -m elf_x86_64 -z noexecstack -T $(KERNEL_DIR)/linker.ld -nostdlib -o $(KERNEL_ELF) $(KERNEL_OBJS)

$(KERNEL_RAW): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_RAW)

$(KERNEL_BIN): $(KERNEL_RAW)
	@size=$$(stat -c%s $(KERNEL_RAW)); \
	if [ "$$size" -gt $(KERNEL_BUDGET_BYTES) ]; then \
		echo "ERROR: Kernel too large"; \
		exit 1; \
	fi
	cp $(KERNEL_RAW) $(KERNEL_BIN)
	truncate -s $(KERNEL_BUDGET_BYTES) $(KERNEL_BIN)

$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) > $(IMAGE)
	dd if=/dev/zero bs=1 count=0 seek=1474560 of=$(IMAGE) 2>/dev/null

run: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE),if=floppy -boot a

run-headless: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE),if=floppy -boot a -display none -serial mon:stdio

clean:
	rm -rf $(BUILD_DIR)
