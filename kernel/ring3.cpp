#include "ring3.hpp"

#include "heap.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "vga.hpp"

extern "C" uint8_t user_program_start[];
extern "C" uint8_t user_program_end[];

extern "C" uint8_t stack_overflow_program_start[];
extern "C" uint8_t stack_overflow_program_end[];

namespace ring3 {

namespace {

constexpr uintptr_t TSS_INFO_ADDR = 0x8FE0;

constexpr uintptr_t USER_CODE_VIRT  = 0x40001000;
constexpr uintptr_t USER_STACK_VIRT = 0x40010000;
constexpr uintptr_t USER_STACK_SIZE = 4096;

constexpr uintptr_t STACK_OVERFLOW_CODE_VIRT = 0x40021000;
constexpr uintptr_t STACK_OVERFLOW_STACK_VIRT = 0x40030000;
constexpr uintptr_t STACK_OVERFLOW_STACK_SIZE = 4096;

constexpr size_t KERNEL_STACK_FOR_RSP0_SIZE = 8192;

bool tss_ready = false;

}

void set_kernel_stack(uint64_t rsp0_top) {

    auto* info = reinterpret_cast<volatile uint64_t*>(TSS_INFO_ADDR);
    const uint64_t tss_addr = info[0];

    auto* tss_bytes = reinterpret_cast<volatile uint8_t*>(tss_addr);
    *reinterpret_cast<volatile uint64_t*>(tss_bytes + 4) = rsp0_top;
}

void setup_tss() {
    if (tss_ready) return;

    auto* info = reinterpret_cast<volatile uint64_t*>(TSS_INFO_ADDR);
    const uint64_t tss_addr = info[0];
    const uint64_t gdt_tss_addr = info[1];
    const uint16_t tss_selector = static_cast<uint16_t>(info[2]);

    void* rsp0_stack = heap::kmalloc(KERNEL_STACK_FOR_RSP0_SIZE);
    const uint64_t rsp0_top = reinterpret_cast<uint64_t>(rsp0_stack) + KERNEL_STACK_FOR_RSP0_SIZE;
    set_kernel_stack(rsp0_top);

    auto* desc = reinterpret_cast<volatile uint8_t*>(gdt_tss_addr);
    *reinterpret_cast<volatile uint16_t*>(desc + 2) = static_cast<uint16_t>(tss_addr & 0xFFFF);
    desc[4] = static_cast<uint8_t>((tss_addr >> 16) & 0xFF);
    desc[7] = static_cast<uint8_t>((tss_addr >> 24) & 0xFF);
    *reinterpret_cast<volatile uint32_t*>(desc + 8) = static_cast<uint32_t>((tss_addr >> 32) & 0xFFFFFFFF);

    asm volatile("ltr %0" : : "r"(tss_selector));

    tss_ready = true;
}

[[noreturn]] void enter(uintptr_t entry_virt, uintptr_t stack_top) {

    const uint64_t rflags = 0x202;
    asm volatile(
        "cli\n"
        "push %0\n"
        "push %1\n"
        "push %2\n"
        "push %3\n"
        "push %4\n"
        "iretq\n"
        :
        : "r"(static_cast<uint64_t>(USER_DATA_SEL)),
          "r"(stack_top),
          "r"(rflags),
          "r"(static_cast<uint64_t>(USER_CODE_SEL)),
          "r"(entry_virt)
        : "memory"
    );

    __builtin_unreachable();
}

void run_demo() {
    setup_tss();

    const size_t program_size = static_cast<size_t>(user_program_end - user_program_start);

    vga::print("Mapping user program (");

    {
        char digits[21];
        int index = 20;
        digits[index] = '\0';
        uint64_t value = program_size;
        if (value == 0) {
            digits[--index] = '0';
        } else {
            while (value != 0) {
                digits[--index] = static_cast<char>('0' + (value % 10));
                value /= 10;
            }
        }
        vga::print(&digits[index]);
    }
    vga::print(" bytes) into a ring 3 page...\n");

    uintptr_t code_frame = pmm::alloc_frame();
    if (code_frame == 0 || !paging::map_page(USER_CODE_VIRT, code_frame,
            paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER | paging::PAGE_NX)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to map the user code page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    uint8_t* dest = reinterpret_cast<uint8_t*>(USER_CODE_VIRT);
    for (size_t i = 0; i < program_size; ++i) {
        dest[i] = user_program_start[i];
    }

    if (!paging::protect_page(USER_CODE_VIRT, paging::PAGE_PRESENT | paging::PAGE_USER)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to re-protect the user code page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    uintptr_t stack_frame = pmm::alloc_frame();
    if (stack_frame == 0 || !paging::map_page(USER_STACK_VIRT, stack_frame,
            paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER | paging::PAGE_NX)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to map the user stack page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    const uint64_t user_stack_top = USER_STACK_VIRT + USER_STACK_SIZE;

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    vga::print("Entering ring 3 now (CS=0x");
    {
        constexpr char hex_digits[] = "0123456789ABCDEF";
        for (int shift = 4; shift >= 0; shift -= 4) {
            vga::put_char(hex_digits[(USER_CODE_SEL >> shift) & 0xF]);
        }
    }
    vga::print(")...\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    enter(USER_CODE_VIRT, user_stack_top);
}

void run_stack_overflow_demo() {
    setup_tss();

    const size_t program_size = static_cast<size_t>(stack_overflow_program_end - stack_overflow_program_start);

    uintptr_t code_frame = pmm::alloc_frame();
    if (code_frame == 0 || !paging::map_page(STACK_OVERFLOW_CODE_VIRT, code_frame,
            paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER | paging::PAGE_NX)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to map the stack-overflow demo's code page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    uint8_t* dest = reinterpret_cast<uint8_t*>(STACK_OVERFLOW_CODE_VIRT);
    for (size_t i = 0; i < program_size; ++i) {
        dest[i] = stack_overflow_program_start[i];
    }

    if (!paging::protect_page(STACK_OVERFLOW_CODE_VIRT, paging::PAGE_PRESENT | paging::PAGE_USER)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to re-protect the stack-overflow demo's code page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    uintptr_t stack_frame = pmm::alloc_frame();
    if (stack_frame == 0 || !paging::map_page(STACK_OVERFLOW_STACK_VIRT, stack_frame,
            paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER | paging::PAGE_NX)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Failed to map the stack-overflow demo's stack page. Aborting.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    vga::print("Entering ring 3 now - this program will deliberately overflow its stack...\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    enter(STACK_OVERFLOW_CODE_VIRT, STACK_OVERFLOW_STACK_VIRT + STACK_OVERFLOW_STACK_SIZE);
}

bool self_test() {
    setup_tss();

    void* scratch = heap::kmalloc(256);
    if (scratch == nullptr) return false;
    uint64_t test_value = reinterpret_cast<uint64_t>(scratch) + 256;

    set_kernel_stack(test_value);

    auto* info = reinterpret_cast<volatile uint64_t*>(TSS_INFO_ADDR);
    const uint64_t tss_addr = info[0];
    auto* tss_bytes = reinterpret_cast<volatile uint8_t*>(tss_addr);
    uint64_t readback = *reinterpret_cast<volatile uint64_t*>(tss_bytes + 4);

    bool ok = (readback == test_value);

    heap::kfree(scratch);

    return ok;
}

}
