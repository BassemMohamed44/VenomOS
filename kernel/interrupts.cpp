#include "interrupts.hpp"

#include "../include/io.hpp"
#include "fs.hpp"
#include "keyboard.hpp"
#include "process.hpp"
#include "ring3.hpp"
#include "scheduler.hpp"
#include "task.hpp"
#include "usercopy.hpp"
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
constexpr uint64_t VECTOR_PAGE_FAULT = 14;
constexpr uint64_t VECTOR_SYSCALL = 0x80;

constexpr uint64_t SYS_EXIT  = 0;
constexpr uint64_t SYS_WRITE = 1;
constexpr uint64_t SYS_SLEEP = 2;
constexpr uint64_t SYS_GETPID = 3;     // () -> pid
constexpr uint64_t SYS_GETPPID = 4;    // () -> parent pid
constexpr uint64_t SYS_UPTIME = 5;     // () -> timer ticks since boot
constexpr uint64_t SYS_READ = 6;       // (fd, buf, len) -> bytes read; fd 0 = keyboard, non-blocking
constexpr uint64_t SYS_FILESIZE = 7;   // (path) -> size in bytes, or -1
constexpr uint64_t SYS_READFILE = 8;   // (path, buf, len) -> bytes copied (at most 4096), or -1
constexpr uint64_t SYS_SPAWN = 9;      // (path, args or 0) -> child pid, or -1
constexpr uint64_t SYS_WAIT = 10;      // (status_ptr or 0) -> pid of a finished child, or -1 if none

constexpr uint64_t SYS_WRITE_MAX_LENGTH = 1024ull * 1024ull;

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

[[noreturn]] void report_ring3_page_fault_and_halt(const interrupts::InterruptFrame* frame) {
    uint64_t faulting_addr;
    asm volatile("mov %%cr2, %0" : "=r"(faulting_addr));

    uintptr_t guard_page = faulting_addr & ~static_cast<uint64_t>(0xFFF);
    bool is_stack_guard = (guard_page == process::STACK_GUARD_VIRT) ||
                          (guard_page == ring3::STACK_GUARD_VIRT) ||
                          (guard_page == ring3::STACK_OVERFLOW_GUARD_VIRT);

    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    if (is_stack_guard) {
        vga::print("\n\nStack overflow detected: ring 3 code ran off the bottom of its stack\n");
        vga::print("and hit the unmapped guard page at 0x");
        print_hex(guard_page);
        vga::print(".\n");
    } else {
        vga::print("\n\nRing 3 page fault at address 0x");
        print_hex(faulting_addr);
        vga::print(" (error code 0x");
        print_hex(frame->error_code);
        vga::print(").\n");
    }
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    vga::print("Terminating the faulting task and returning control to the scheduler...\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    task::exit_current(-1);

    for (;;) {
        asm volatile("cli; hlt");
    }
}

constexpr uint64_t SYS_ERROR = static_cast<uint64_t>(-1);
constexpr size_t SYS_READFILE_MAX = 4096;

// Copies a NUL-terminated file name out of user memory, one byte at a time so
// every byte is validated. Fails if it is unmapped, empty or too long.
bool copy_user_path(uintptr_t user_ptr, char* out, size_t capacity) {
    for (size_t i = 0; i < capacity; ++i) {
        char c;
        if (!usercopy::copy_from_user(&c, reinterpret_cast<const void*>(user_ptr + i), 1)) return false;
        out[i] = c;
        if (c == '\0') return i > 0;
    }
    return false;
}

void handle_syscall(interrupts::InterruptFrame* frame) {
    switch (frame->rax) {
        case SYS_GETPID: {
            const task::Task* self = task::current();
            frame->rax = self != nullptr ? static_cast<uint64_t>(self->pid) : SYS_ERROR;
            break;
        }
        case SYS_GETPPID: {
            const task::Task* self = task::current();
            frame->rax = self != nullptr ? static_cast<uint64_t>(self->parent_pid) : SYS_ERROR;
            break;
        }
        case SYS_UPTIME: {
            frame->rax = interrupts::ticks();
            break;
        }
        case SYS_READ: {
            const uint64_t fd = frame->rdi;
            uintptr_t user_buf = static_cast<uintptr_t>(frame->rsi);
            uint64_t length = frame->rdx;
            if (fd != 0 || length > 256) { frame->rax = SYS_ERROR; break; }
            uint8_t chunk[256];
            size_t got = 0;
            while (got < length && keyboard::available()) {
                chunk[got++] = static_cast<uint8_t>(keyboard::read_char());
            }
            if (got > 0 && !usercopy::copy_to_user(reinterpret_cast<void*>(user_buf), chunk, got)) {
                frame->rax = SYS_ERROR;
                break;
            }
            frame->rax = got;
            break;
        }
        case SYS_FILESIZE: {
            char name[fs::MAX_FILENAME + 1];
            if (!copy_user_path(static_cast<uintptr_t>(frame->rdi), name, sizeof(name))) { frame->rax = SYS_ERROR; break; }
            const int64_t size = fs::file_size(name);
            frame->rax = size < 0 ? SYS_ERROR : static_cast<uint64_t>(size);
            break;
        }
        case SYS_SPAWN: {
            char program[40];
            if (!copy_user_path(static_cast<uintptr_t>(frame->rdi), program, sizeof(program))) { frame->rax = SYS_ERROR; break; }
            char args[160];
            const char* args_ptr = nullptr;
            if (frame->rsi != 0) {
                size_t n = 0;
                for (; n < sizeof(args); ++n) {
                    char c;
                    if (!usercopy::copy_from_user(&c, reinterpret_cast<const void*>(static_cast<uintptr_t>(frame->rsi) + n), 1)) { n = sizeof(args); break; }
                    args[n] = c;
                    if (c == '\0') break;
                }
                if (n >= sizeof(args)) { frame->rax = SYS_ERROR; break; }
                args_ptr = args;
            }
            uint64_t child = 0;
            frame->rax = process::spawn_line(program, args_ptr, &child) ? child : SYS_ERROR;
            break;
        }
        case SYS_WAIT: {
            task::Pid pid = 0;
            int code = 0;
            if (!task::wait_for_child(&pid, &code)) { frame->rax = SYS_ERROR; break; }
            if (frame->rdi != 0) {
                const int32_t status = code;
                usercopy::copy_to_user(reinterpret_cast<void*>(static_cast<uintptr_t>(frame->rdi)), &status, sizeof(status));
            }
            frame->rax = pid;
            break;
        }
        case SYS_READFILE: {
            char name[fs::MAX_FILENAME + 1];
            uintptr_t user_buf = static_cast<uintptr_t>(frame->rsi);
            uint64_t length = frame->rdx;
            if (!copy_user_path(static_cast<uintptr_t>(frame->rdi), name, sizeof(name))) { frame->rax = SYS_ERROR; break; }
            if (length > SYS_READFILE_MAX) length = SYS_READFILE_MAX;
            if (!usercopy::validate_user_range(user_buf, static_cast<size_t>(length), true)) { frame->rax = SYS_ERROR; break; }
            static uint8_t file_chunk[SYS_READFILE_MAX];
            size_t got = 0;
            if (!fs::read(name, file_chunk, static_cast<size_t>(length), &got)) { frame->rax = SYS_ERROR; break; }
            if (got > 0 && !usercopy::copy_to_user(reinterpret_cast<void*>(user_buf), file_chunk, got)) {
                frame->rax = SYS_ERROR;
                break;
            }
            frame->rax = got;
            break;
        }
        case SYS_EXIT: {
            int exit_code = static_cast<int>(frame->rdi);
            task::exit_current(exit_code);
            break;
        }
        case SYS_WRITE: {
            uintptr_t user_ptr = static_cast<uintptr_t>(frame->rdi);
            uint64_t length = frame->rsi;

            if (length > SYS_WRITE_MAX_LENGTH) {
                frame->rax = static_cast<uint64_t>(-1);
                break;
            }

            constexpr size_t CHUNK_SIZE = 256;
            uint8_t chunk[CHUNK_SIZE];

            uint64_t written = 0;
            while (written < length) {
                uint64_t remaining = length - written;
                size_t this_chunk = remaining < CHUNK_SIZE ? static_cast<size_t>(remaining) : CHUNK_SIZE;

                if (!usercopy::copy_from_user(chunk, reinterpret_cast<const void*>(user_ptr + written), this_chunk)) {

                    frame->rax = (written > 0) ? written : static_cast<uint64_t>(-1);
                    return;
                }

                for (size_t i = 0; i < this_chunk; ++i) {
                    vga::put_char(static_cast<char>(chunk[i]));
                }
                written += this_chunk;
            }

            frame->rax = written;
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

    set_gate(static_cast<uint8_t>(VECTOR_SYSCALL), isr_stub_table[VECTOR_SYSCALL], 3);

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

bool syscall_self_test() {
    InterruptFrame f = {};
    bool ok = true;

    f.rax = SYS_GETPID;
    handle_syscall(&f);
    ok = ok && task::current() != nullptr && f.rax == static_cast<uint64_t>(task::current()->pid);

    f = {};
    f.rax = SYS_GETPPID;
    handle_syscall(&f);
    ok = ok && f.rax == static_cast<uint64_t>(task::current()->parent_pid);

    f = {};
    f.rax = SYS_UPTIME;
    handle_syscall(&f);
    ok = ok && f.rax <= ticks();

    // Unknown syscall numbers and kernel-space pointers are rejected.
    f = {};
    f.rax = 999;
    handle_syscall(&f);
    ok = ok && f.rax == SYS_ERROR;

    static const char kernel_path[] = "hello.elf";
    f = {};
    f.rax = SYS_FILESIZE;
    f.rdi = reinterpret_cast<uint64_t>(kernel_path);   // not a user address
    handle_syscall(&f);
    ok = ok && f.rax == SYS_ERROR;

    f = {};
    f.rax = SYS_READ;      // fd 1 is not readable
    f.rdi = 1;
    handle_syscall(&f);
    ok = ok && f.rax == SYS_ERROR;

    return ok;
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
        if (frame->vector == VECTOR_PAGE_FAULT && (frame->cs & 0x3) == 3) {
            report_ring3_page_fault_and_halt(frame);
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
