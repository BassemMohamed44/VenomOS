#pragma once

#include "../include/stdint.hpp"

namespace interrupts {

// Installs the 256-entry IDT, remaps the PIC away from CPU exception
// vectors, and enables the timer and keyboard IRQ lines.
void init();
uint64_t ticks();

// Called from the assembly interrupt entry stubs after they have saved the
// interrupted general-purpose register state.
extern "C" void interrupt_dispatch(uint64_t vector, uint64_t error_code);

} // namespace interrupts
