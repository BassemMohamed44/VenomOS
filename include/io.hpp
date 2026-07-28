// ============================================================
// io.hpp
//
// Thin wrappers around the x86 IN/OUT instructions. These talk
// directly to hardware device registers (as opposed to memory) -
// used here to program the VGA hardware cursor. Kept header-only
// and inline since there is no libc/runtime to link against.
// ============================================================
#pragma once

#include "stdint.hpp"
#include "stddef.hpp"

namespace io {

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

} // namespace io
