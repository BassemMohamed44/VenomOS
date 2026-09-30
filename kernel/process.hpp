#pragma once

#include "../include/stdint.hpp"

namespace process {

constexpr uintptr_t STACK_GUARD_VIRT = 0x7FFFE000;
constexpr uintptr_t STACK_VIRT = 0x7FFFF000;
constexpr uintptr_t STACK_SIZE = 4 * 4096;

bool run(const char* filename);

bool self_test();

}
