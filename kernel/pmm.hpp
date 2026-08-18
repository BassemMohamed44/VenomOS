#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace pmm {

constexpr uint64_t FRAME_SIZE = 4096;

void init();

uintptr_t alloc_frame();


void free_frame(uintptr_t phys_addr);

uint64_t total_frames();
uint64_t used_frames();
uint64_t free_frames();

}
