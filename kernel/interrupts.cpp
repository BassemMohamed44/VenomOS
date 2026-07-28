#include "interrupts.hpp"
#include "../include/io.hpp"
#include "keyboard.hpp"
#include "vga.hpp"

extern "C" void* isr_stub_table[];

namespace {

constexpr uint8_t PIC1_COMMAND = 0x20;
constexpr uint8_t PIC1_DATA = 0x21;
constexpr uint8_t PIC2_COMMAND = 0xA0;
constexpr uint8_t PIC2_DATA = 0xA1;
constexpr uint8_t PIC_EOI = 0x20;
constexpr uint8_t PIC_ICW1_INIT = 0x10;
constexpr uint8_t PIC_ICW1_ICW4 = 0x01;
constexpr uint8_t PIC_ICW4_8086 = 0x01;
constexpr uint8_t IRQ_BASE = 32;
constexpr uint16_t KERNEL_CODE_SELECTOR = 0x18;

struct [[gnu::packed]] IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
};

struct [[gnu::packed]] IdtPointer {
    uint16_t limit;
    uint64_t base;
};

IdtEntry idt[256] = {};
volatile uint64_t timer_ticks = 0;

void set_gate(uint8_t vector, void* handler) {
    const uint64_t address = reinterpret_cast<uint64_t>(handler);
    IdtEntry& entry = idt[vector];
    entry.offset_low = static_cast<uint16_t>(address);
    entry.selector = KERNEL_CODE_SELECTOR;
    entry.ist = 0;
    entry.type_attributes = 0x8E; // present, ring 0, interrupt gate
    entry.offset_middle = static_cast<uint16_t>(address >> 16);
    entry.offset_high = static_cast<uint32_t>(address >> 32);
    entry.reserved = 0;
}

void remap_pic() {
    io::outb(PIC1_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io::outb(PIC2_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io::outb(PIC1_DATA, IRQ_BASE);
    io::outb(PIC2_DATA, IRQ_BASE + 8);
    io::outb(PIC1_DATA, 0x04); // slave PIC is wired to master IRQ2
    io::outb(PIC2_DATA, 0x02);
    io::outb(PIC1_DATA, PIC_ICW4_8086);
    io::outb(PIC2_DATA, PIC_ICW4_8086);

    // Enable only IRQ0 (timer) and IRQ1 (keyboard). Keeping every other line
    // masked prevents unhandled legacy devices from interrupting the kernel.
    io::outb(PIC1_DATA, 0xFC);
    io::outb(PIC2_DATA, 0xFF);
}

void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        io::outb(PIC2_COMMAND, PIC_EOI);
    }
    io::outb(PIC1_COMMAND, PIC_EOI);
}

void print_hex(uint64_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4) {
        vga::put_char(digits[(value >> shift) & 0xF]);
    }
}

[[noreturn]] void halt_after_exception(uint64_t vector, uint64_t error_code) {
    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    vga::print("\nCPU exception 0x");
    print_hex(vector);
    vga::print(" error 0x");
    print_hex(error_code);
    vga::print(". System halted.\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

} // namespace

namespace interrupts {

void init() {
    for (uint16_t vector = 0; vector < 256; ++vector) {
        set_gate(static_cast<uint8_t>(vector), isr_stub_table[vector]);
    }

    const IdtPointer pointer = {
        static_cast<uint16_t>(sizeof(idt) - 1),
        reinterpret_cast<uint64_t>(&idt[0]),
    };
    asm volatile("lidt %0" : : "m"(pointer));

    remap_pic();
}

uint64_t ticks() {
    return timer_ticks;
}

extern "C" void interrupt_dispatch(uint64_t vector, uint64_t error_code) {
    if (vector < IRQ_BASE) {
        halt_after_exception(vector, error_code);
    }
    if (vector >= IRQ_BASE + 16) {
        return;
    }

    const uint8_t irq = static_cast<uint8_t>(vector - IRQ_BASE);
    if (irq == 0) {
        ++timer_ticks;
    } else if (irq == 1) {
        keyboard::handle_irq();
    }

    send_eoi(irq);
}

} // namespace interrupts
