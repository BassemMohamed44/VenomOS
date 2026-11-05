#pragma once

#include "../include/stdint.hpp"

namespace paging {

constexpr uint64_t PAGE_PRESENT  = 1ull << 0;
constexpr uint64_t PAGE_WRITABLE = 1ull << 1;
constexpr uint64_t PAGE_USER     = 1ull << 2;
constexpr uint64_t PAGE_OWNED    = 1ull << 9;
constexpr uint64_t PAGE_NX       = 1ull << 63;

bool map_page(uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE);

bool map_page_in(uint64_t pml4_phys, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE);

bool unmap_page(uintptr_t virt_addr);

bool protect_page(uintptr_t virt_addr, uint64_t new_flags);

uint64_t create_address_space();

void destroy_address_space(uint64_t pml4_phys);

// True when EFER.NXE is set, i.e. the CPU honours PAGE_NX.
bool nx_enabled();

// Counts present pages in the user half of an address space that are both
// writable and executable (W^X violations). 0 means the space obeys W^X.
uint64_t count_wx_pages(uint64_t pml4_phys);

// Splits the kernel's first 2 MiB huge page into 4 KiB pages so the kernel
// image itself obeys W^X (code R+X, rodata R+NX, data/bss/stack RW+NX), then
// sets CR0.WP so ring 0 cannot write through read-only pages. Call once,
// after pmm::init().
bool protect_kernel_image();

bool self_test();

}
