// ============================================================
// kernel.cpp
//
// This is the first C++ code that runs in VenomOS. It is called
// by kernel_entry.asm's _start once a stack is set up. By this
// point Stage 2 has already completed the real Real Mode ->
// Protected Mode -> Long Mode transition described in boot/stage2.asm,
// printing its own genuine progress messages along the way - those
// messages are already sitting on screen right now, for real.
//
// kernel_main() pauses for a moment (using the REAL hardware timer,
// via interrupts::ticks() - not a fabricated delay) so there's time
// to actually read that boot log before clearing the screen to show
// the shell. This is different from an earlier version of this file,
// which used to clear the screen immediately and then reprint a fake
// copy of the boot messages with an artificial delay to make it look
// like they were "replaying" live. That was dishonest: the text was
// invented, not real. This version prints nothing fake - it just
// waits, using genuine elapsed time, before moving on.
// ============================================================
#include "keyboard.hpp"
#include "interrupts.hpp"
#include "kernel.hpp"
#include "shell.hpp"
#include "vga.hpp"

namespace {

// Blocks until at least `ticks_to_wait` real timer interrupts (IRQ0)
// have fired. The PIT's default rate is ~18.2 Hz, so this is a real,
// measured pause - not a busy-loop guess at CPU speed.
void wait_real_ticks(uint64_t ticks_to_wait) {
    const uint64_t start = interrupts::ticks();
    while (interrupts::ticks() - start < ticks_to_wait) {
        asm volatile("hlt" : : : "memory");
    }
}

} // namespace

extern "C" void kernel_main()
{
    vga::init();

    // Interrupts must be live before wait_real_ticks() below can measure
    // anything (it relies on the timer IRQ actually firing), so bring up
    // the IDT/PIC and enable interrupts now, while Stage 1/Stage 2's real
    // boot log is still the only thing on screen.
    keyboard::init();
    interrupts::init();
    asm volatile("sti");

    // Give a real ~3 seconds (about 54 ticks at ~18.2 Hz) to actually read
    // the genuine boot log already on screen before clearing it.
    wait_real_ticks(54);

    vga::clear(vga::Color::LightGrey, vga::Color::Black);

    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::print("VenomOS Kernel: running in 64-bit.\n\n");

    vga::set_color(vga::Color::White, vga::Color::Black);
    vga::print("Version      : 0.2.0\n");
    vga::print("Architecture : x86_64\n");
    vga::print("Kernel       : Running\n\n");
    shell::init();

    while (true)
    {
        asm volatile("hlt" : : : "memory");
        // IRQ1 normally queues input immediately. Polling after every timer
        // wake also handles PS/2 controllers that do not raise IRQ1.
        keyboard::poll();
        shell::tick();
        while (keyboard::available()) {
            shell::handle_char(keyboard::read_char());
        }
    }
}
