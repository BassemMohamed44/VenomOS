#include "interrupts.hpp"

#include "../include/io.hpp"
#include "keyboard.hpp"
#include "scheduler.hpp"
#include "task.hpp"
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

constexpr uint64_t VECTOR_GENERAL_PROTECTION_FAULT = 13;
constexpr uint64_t VECTOR_SYSCALL = 0x80;

constexpr uint64_t SYS_EXIT  = 0;
constexpr uint64_t SYS_WRITE = 1;
constexpr uint64_t SYS_SLEEP = 2; 

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

void set_gate(uint8_t vector, void* handler, uint8_t dpl = 0) {
    const uint64_t address = reinterpret_cast<uint64_t>(handler);
    IdtEntry& entry = idt[vector];
    entry.offset_low = static_cast<uint16_t>(address);
    entry.selector = KERNEL_CODE_SELECTOR;
    entry.ist = 0;
    entry.type_attributes = static_cast<uint8_t>(0x8E | (dpl << 5)); 
    entry.offset_middle = static_cast<uint16_t>(address >> 16);
    entry.offset_high = static_cast<uint32_t>(address >> 32);
    entry.reserved = 0;
}

void remap_pic() {
    io::outb(PIC1_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io::outb(PIC2_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io::outb(PIC1_DATA, IRQ_BASE);
    io::outb(PIC2_DATA, IRQ_BASE + 8);
    io::outb(PIC1_DATA, 0x04); 
    io::outb(PIC2_DATA, 0x02);
    io::outb(PIC1_DATA, PIC_ICW4_8086);
    io::outb(PIC2_DATA, PIC_ICW4_8086);

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

[[noreturn]] void report_ring3_fault_and_halt(const interrupts::InterruptFrame* frame) {
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::print("\n\nRing 3 privilege violation caught by the CPU as a real #GP fault.\n");
    vga::print("Faulting instruction attempted a privileged operation from CS=0x");
    print_hex(frame->cs);
    vga::print(" (RPL=");
    vga::put_char(static_cast<char>('0' + (frame->cs & 0x3)));
    vga::print(") - user mode code cannot execute privileged instructions.\n");
    vga::print("This proves ring 0 / ring 3 separation is genuinely enforced by the CPU.\n\n");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    vga::print("Terminating the faulting task and returning control to the scheduler...\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    task::exit_current(-1); 

    for (;;) {
        asm volatile("cli; hlt");
    }
}


void handle_syscall(interrupts::InterruptFrame* frame) {
    switch (frame->rax) {
        case SYS_EXIT: {
            int exit_code = static_cast<int>(frame->rdi);
            task::exit_current(exit_code); 
            break; 
        }
        case SYS_WRITE: {
            const char* buffer = reinterpret_cast<const char*>(frame->rdi);
            uint64_t length = frame->rsi;
            for (uint64_t i = 0; i < length; ++i) {
                vga::put_char(buffer[i]);
            }
            frame->rax = length; 
            break;
        }
        case SYS_SLEEP: {
            task::sleep_current(frame->rdi);
            frame->rax = 0; 
            break;
        }
        default:
            frame->rax = static_cast<uint64_t>(-1); 
            break;
    }
}

} 

namespace interrupts {

void init() {
    for (uint16_t vector = 0; vector < 256; ++vector) {
        set_gate(static_cast<uint8_t>(vector), isr_stub_table[vector]);
    }

    set_gate(static_cast<uint8_t>(VECTOR_SYSCALL), isr_stub_table[VECTOR_SYSCALL], /*dpl=*/3);

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

extern "C" void interrupt_dispatch(InterruptFrame* frame) {
    if (frame->vector == VECTOR_SYSCALL) {
        handle_syscall(frame);
        return;
    }

    if (frame->vector < IRQ_BASE) {
        if (frame->vector == VECTOR_GENERAL_PROTECTION_FAULT && (frame->cs & 0x3) == 3) {
            report_ring3_fault_and_halt(frame);
        }
        halt_after_exception(frame->vector, frame->error_code);
    }

    if (frame->vector >= IRQ_BASE && frame->vector < IRQ_BASE + 16) {
        const uint8_t irq = static_cast<uint8_t>(frame->vector - IRQ_BASE);
        if (irq == 0) {
            ++timer_ticks;
            send_eoi(irq); 
            scheduler::tick();
            return;
        } else if (irq == 1) {
            keyboard::handle_irq();
        }
        send_eoi(irq);
    }
}

} 
