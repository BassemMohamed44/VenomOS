#include "usercopy.hpp"
#include "paging.hpp"
#include "elf.hpp"
#include "pmm.hpp"

namespace usercopy {

namespace {

constexpr uint64_t PAGE_SIZE = 0x1000ull;
constexpr uint64_t PAGE_MASK = ~(PAGE_SIZE - 1);

constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ull;
constexpr uint64_t PAGE_HUGE = 1ull << 7;

inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline uint64_t* table_ptr(uint64_t phys_addr) {
    return reinterpret_cast<uint64_t*>(phys_addr);
}

bool page_is_user_accessible(uintptr_t page_addr, bool require_writable) {
    uint64_t pml4_index = (page_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (page_addr >> 30) & 0x1FF;
    uint64_t pd_index   = (page_addr >> 21) & 0x1FF;
    uint64_t pt_index   = (page_addr >> 12) & 0x1FF;

    uint64_t* pml4 = table_ptr(read_cr3() & ADDR_MASK);
    uint64_t pml4_entry = pml4[pml4_index];
    if (!(pml4_entry & paging::PAGE_PRESENT) || !(pml4_entry & paging::PAGE_USER)) return false;

    uint64_t* pdpt = table_ptr(pml4_entry & ADDR_MASK);
    uint64_t pdpt_entry = pdpt[pdpt_index];
    if (!(pdpt_entry & paging::PAGE_PRESENT) || !(pdpt_entry & paging::PAGE_USER)) return false;
    if (pdpt_entry & PAGE_HUGE) {
        return !require_writable || (pdpt_entry & paging::PAGE_WRITABLE);
    }

    uint64_t* pd = table_ptr(pdpt_entry & ADDR_MASK);
    uint64_t pd_entry = pd[pd_index];
    if (!(pd_entry & paging::PAGE_PRESENT) || !(pd_entry & paging::PAGE_USER)) return false;
    if (pd_entry & PAGE_HUGE) {
        return !require_writable || (pd_entry & paging::PAGE_WRITABLE);
    }

    uint64_t* pt = table_ptr(pd_entry & ADDR_MASK);
    uint64_t pt_entry = pt[pt_index];
    if (!(pt_entry & paging::PAGE_PRESENT) || !(pt_entry & paging::PAGE_USER)) return false;

    return !require_writable || (pt_entry & paging::PAGE_WRITABLE);
}

}

bool validate_user_range(uintptr_t addr, size_t length, bool require_writable) {
    if (length == 0) return true;

    uintptr_t end = addr + static_cast<uintptr_t>(length);
    if (end < addr) return false;

    if (addr < elf::USER_SPACE_MIN) return false;
    if (end > elf::USER_SPACE_MAX) return false;

    uintptr_t first_page = addr & PAGE_MASK;
    uintptr_t last_page  = (end - 1) & PAGE_MASK;

    for (uintptr_t page = first_page; ; page += PAGE_SIZE) {
        if (!page_is_user_accessible(page, require_writable)) return false;
        if (page == last_page) break;
    }

    return true;
}

bool copy_from_user(void* kernel_dst, const void* user_src, size_t length) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(user_src);
    if (!validate_user_range(addr, length, false)) return false;

    const uint8_t* src = reinterpret_cast<const uint8_t*>(user_src);
    uint8_t* dst = reinterpret_cast<uint8_t*>(kernel_dst);
    for (size_t i = 0; i < length; ++i) dst[i] = src[i];
    return true;
}

bool copy_to_user(void* user_dst, const void* kernel_src, size_t length) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(user_dst);
    if (!validate_user_range(addr, length, true)) return false;

    uint8_t* dst = reinterpret_cast<uint8_t*>(user_dst);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(kernel_src);
    for (size_t i = 0; i < length; ++i) dst[i] = src[i];
    return true;
}

bool self_test() {
    bool ok = true;

    constexpr uintptr_t VALID_ADDR      = elf::USER_SPACE_MIN + 0x100000;
    constexpr uintptr_t NOT_USER_ADDR   = elf::USER_SPACE_MIN + 0x200000;
    constexpr uintptr_t UNMAPPED_ADDR   = elf::USER_SPACE_MIN + 0x300000;

    uintptr_t valid_frame = pmm::alloc_frame();
    if (valid_frame == 0) return false;

    if (!paging::map_page(VALID_ADDR, valid_frame,
                           paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER)) {
        pmm::free_frame(valid_frame);
        return false;
    }

    uint8_t* valid_ptr = reinterpret_cast<uint8_t*>(VALID_ADDR);
    for (int i = 0; i < 16; ++i) valid_ptr[i] = static_cast<uint8_t>(0x50 + i);

    uint8_t readback[16] = {};
    if (!copy_from_user(readback, reinterpret_cast<const void*>(VALID_ADDR), sizeof(readback))) ok = false;
    for (int i = 0; i < 16 && ok; ++i) {
        if (readback[i] != static_cast<uint8_t>(0x50 + i)) ok = false;
    }

    uintptr_t kernel_only_frame = pmm::alloc_frame();
    if (kernel_only_frame == 0) {
        ok = false;
    } else {
        if (!paging::map_page(NOT_USER_ADDR, kernel_only_frame,
                               paging::PAGE_PRESENT | paging::PAGE_WRITABLE)) {
            ok = false;
        } else {
            if (copy_from_user(readback, reinterpret_cast<const void*>(NOT_USER_ADDR), sizeof(readback))) {
                ok = false;
            }
            paging::unmap_page(NOT_USER_ADDR);
        }
        pmm::free_frame(kernel_only_frame);
    }

    if (copy_from_user(readback, reinterpret_cast<const void*>(UNMAPPED_ADDR), sizeof(readback))) {
        ok = false;
    }

    if (copy_from_user(readback, reinterpret_cast<const void*>(0), sizeof(readback))) ok = false;
    if (copy_from_user(readback, reinterpret_cast<const void*>(0x1000), sizeof(readback))) ok = false;

    uintptr_t near_top = elf::USER_SPACE_MAX - 8;
    size_t huge_length = static_cast<size_t>(~0ull);
    if (copy_from_user(readback, reinterpret_cast<const void*>(near_top), huge_length)) ok = false;

    if (!copy_from_user(readback, reinterpret_cast<const void*>(0), 0)) ok = false;

    paging::unmap_page(VALID_ADDR);
    pmm::free_frame(valid_frame);

    return ok;
}

}
