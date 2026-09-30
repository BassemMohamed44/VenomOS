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

bool self_test();

}
