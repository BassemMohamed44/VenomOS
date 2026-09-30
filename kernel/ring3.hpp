#pragma once

#include "../include/stdint.hpp"

namespace ring3 {

constexpr uint16_t USER_CODE_SEL = 0x20 | 3;
constexpr uint16_t USER_DATA_SEL = 0x28 | 3;

void setup_tss();

void set_kernel_stack(uint64_t rsp0_top);

[[noreturn]] void enter(uintptr_t entry_virt, uintptr_t stack_top);

void run_demo();

bool self_test();

}
