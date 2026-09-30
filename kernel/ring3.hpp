#pragma once

#include "../include/stdint.hpp"

namespace ring3 {

constexpr uintptr_t STACK_GUARD_VIRT = 0x4000F000;
constexpr uintptr_t STACK_OVERFLOW_GUARD_VIRT = 0x4002F000;

constexpr uint16_t USER_CODE_SEL = 0x20 | 3;
constexpr uint16_t USER_DATA_SEL = 0x28 | 3;

void setup_tss();

void set_kernel_stack(uint64_t rsp0_top);

[[noreturn]] void enter(uintptr_t entry_virt, uintptr_t stack_top);

void run_demo();

void run_stack_overflow_demo();

bool self_test();

}
