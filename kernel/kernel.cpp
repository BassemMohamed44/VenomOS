#include "keyboard.hpp"
#include "interrupts.hpp"
#include "kernel.hpp"
#include "paging.hpp"
#include "heap.hpp"
#include "pmm.hpp"
#include "ata.hpp"
#include "fs.hpp"
#include "process.hpp"
#include "shell.hpp"
#include "task.hpp"
#include "vga.hpp"

extern "C" uint8_t demo_hello_start[];
extern "C" uint8_t demo_hello_end[];

namespace {

void wait_real_ticks(uint64_t ticks_to_wait) {
    const uint64_t start = interrupts::ticks();
    while (interrupts::ticks() - start < ticks_to_wait) {
        asm volatile("hlt" : : : "memory");
    }
}

}
extern "C" void kernel_main()
{
    vga::init();

    keyboard::init();
    interrupts::init();
    asm volatile("sti");

    wait_real_ticks(54);

    vga::clear(vga::Color::LightGrey, vga::Color::Black);

    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::print("VenomOS Kernel: kernel_main() running in 64-bit Long Mode.\n\n");

    vga::set_color(vga::Color::White, vga::Color::Black);
    vga::print("Version      : 0.2.0\n");
    vga::print("Architecture : x86_64\n");
    vga::print("Kernel       : Running\n\n");

    pmm::init();
    vga::print("Memory Manager  : initialized (see 'meminfo' for details)\n");

    vga::print("Paging self-test: ");
    if (paging::self_test()) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        vga::print("PASS\n");
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("FAIL\n");
    }
    vga::set_color(vga::Color::White, vga::Color::Black);

    heap::init();
    task::init();
    vga::print("\n");

    vga::print("Disk (ATA)      : ");
    bool disk_ok = ata::init();
    if (disk_ok) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        vga::print("PASS\n");
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("FAIL (no drive found - filesystem commands will not work)\n");
    }
    vga::set_color(vga::Color::White, vga::Color::Black);

    if (disk_ok) {
        fs::init();
        vga::print("Filesystem      : VenomFS ready (see 'ls'/'cat'/'write'/'rm')\n");

        size_t demo_hello_size = static_cast<size_t>(demo_hello_end - demo_hello_start);
        fs::write("hello.elf", demo_hello_start, demo_hello_size);

        vga::print("ELF loader self-test: ");
        if (process::self_test()) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
            vga::print("PASS\n");
        } else {
            vga::set_color(vga::Color::LightRed, vga::Color::Black);
            vga::print("FAIL\n");
        }
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
    vga::print("\n");

    shell::init();

    while (true)
    {
        asm volatile("hlt" : : : "memory");

        keyboard::poll();
        shell::tick();
        while (keyboard::available()) {
            shell::handle_char(keyboard::read_char());
        }
    }
}
