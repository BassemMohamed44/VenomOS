#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace usercopy {

bool validate_user_range(uintptr_t addr, size_t length, bool require_writable);

bool copy_from_user(void* kernel_dst, const void* user_src, size_t length);

bool copy_to_user(void* user_dst, const void* kernel_src, size_t length);

bool self_test();

}
