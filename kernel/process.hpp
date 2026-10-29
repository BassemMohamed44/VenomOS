#pragma once

#include "../include/stdint.hpp"

namespace process {

constexpr uintptr_t STACK_GUARD_VIRT = 0x7FFFE000;
constexpr uintptr_t STACK_VIRT = 0x7FFFF000;
constexpr uintptr_t STACK_SIZE = 4 * 4096;

// Page-table flags for a PT_LOAD segment: writable only if PF_WRITE,
// executable (no PAGE_NX) only if PF_EXEC. Never both (W^X).
uint64_t segment_page_flags(uint32_t p_flags);

bool run(const char* filename);

bool self_test();

}
