#include "paging.hpp"
#include "pmm.hpp"

namespace paging {

namespace {

constexpr uint64_t PAGE_HUGE = 1ull << 7;
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ull;

inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline void invalidate_page(uintptr_t virt_addr) {
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

inline uint64_t* table_ptr(uint64_t phys_addr) {

    return reinterpret_cast<uint64_t*>(phys_addr);
}

uint64_t get_or_create_table(uint64_t* table, uint64_t index, uint64_t flags) {
    const uint64_t extra_flags = flags & ~ADDR_MASK & ~PAGE_HUGE;

    if (table[index] & PAGE_PRESENT) {
        table[index] |= (PAGE_WRITABLE | extra_flags);
        return table[index] & ADDR_MASK;
    }

    uintptr_t new_frame = pmm::alloc_frame();
    if (new_frame == 0) return 0;

    uint64_t* new_table = table_ptr(new_frame);
    for (int i = 0; i < 512; ++i) new_table[i] = 0;

    table[index] = new_frame | PAGE_PRESENT | PAGE_WRITABLE | extra_flags;
    return new_frame;
}

inline bool is_huge(uint64_t entry) {
    return (entry & PAGE_PRESENT) && (entry & PAGE_HUGE);
}

}

bool map_page_in(uint64_t pml4_phys, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags) {
    if (virt_addr % 4096 != 0 || phys_addr % 4096 != 0) return false;

    uint64_t pml4_index = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_index   = (virt_addr >> 21) & 0x1FF;
    uint64_t pt_index   = (virt_addr >> 12) & 0x1FF;

    uint64_t* pml4 = table_ptr(pml4_phys);

    uint64_t pdpt_phys = get_or_create_table(pml4, pml4_index, flags);
    if (pdpt_phys == 0) return false;
    uint64_t* pdpt = table_ptr(pdpt_phys);

    if (is_huge(pdpt[pdpt_index])) return false;

    uint64_t pd_phys = get_or_create_table(pdpt, pdpt_index, flags);
    if (pd_phys == 0) return false;
    uint64_t* pd = table_ptr(pd_phys);

    if (is_huge(pd[pd_index])) return false;

    uint64_t pt_phys = get_or_create_table(pd, pd_index, flags);
    if (pt_phys == 0) return false;
    uint64_t* pt = table_ptr(pt_phys);

    pt[pt_index] = (phys_addr & ADDR_MASK) | (flags | PAGE_PRESENT);

    if (pml4_phys == (read_cr3() & ADDR_MASK)) {
        invalidate_page(virt_addr);
    }
    return true;
}

bool map_page(uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags) {
    return map_page_in(read_cr3() & ADDR_MASK, virt_addr, phys_addr, flags);
}

bool unmap_page(uintptr_t virt_addr) {
    if (virt_addr % 4096 != 0) return false;

    uint64_t pml4_index = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_index   = (virt_addr >> 21) & 0x1FF;
    uint64_t pt_index   = (virt_addr >> 12) & 0x1FF;

    uint64_t* pml4 = table_ptr(read_cr3() & ADDR_MASK);
    if (!(pml4[pml4_index] & PAGE_PRESENT)) return false;

    uint64_t* pdpt = table_ptr(pml4[pml4_index] & ADDR_MASK);
    if (!(pdpt[pdpt_index] & PAGE_PRESENT) || is_huge(pdpt[pdpt_index])) return false;

    uint64_t* pd = table_ptr(pdpt[pdpt_index] & ADDR_MASK);
    if (!(pd[pd_index] & PAGE_PRESENT) || is_huge(pd[pd_index])) return false;

    uint64_t* pt = table_ptr(pd[pd_index] & ADDR_MASK);
    if (!(pt[pt_index] & PAGE_PRESENT)) return false;

    pt[pt_index] = 0;
    invalidate_page(virt_addr);
    return true;
}

uint64_t create_address_space() {
    uint64_t kernel_pml4_phys = read_cr3() & ADDR_MASK;
    uint64_t* kernel_pml4 = table_ptr(kernel_pml4_phys);

    if (!(kernel_pml4[0] & PAGE_PRESENT)) return 0;
    uint64_t* kernel_pdpt = table_ptr(kernel_pml4[0] & ADDR_MASK);
    uint64_t kernel_space_pdpt_entry = kernel_pdpt[0];

    uintptr_t new_pml4_frame = pmm::alloc_frame();
    if (new_pml4_frame == 0) return 0;
    uintptr_t new_pdpt_frame = pmm::alloc_frame();
    if (new_pdpt_frame == 0) {
        pmm::free_frame(new_pml4_frame);
        return 0;
    }

    uint64_t* new_pml4 = table_ptr(new_pml4_frame);
    uint64_t* new_pdpt = table_ptr(new_pdpt_frame);
    for (int i = 0; i < 512; ++i) { new_pml4[i] = 0; new_pdpt[i] = 0; }

    new_pdpt[0] = kernel_space_pdpt_entry;
    new_pml4[0] = new_pdpt_frame | PAGE_PRESENT | PAGE_WRITABLE;

    return new_pml4_frame;
}

void destroy_address_space(uint64_t pml4_phys) {
    uint64_t* pml4 = table_ptr(pml4_phys);
    if (!(pml4[0] & PAGE_PRESENT)) { pmm::free_frame(pml4_phys); return; }

    uint64_t* pdpt = table_ptr(pml4[0] & ADDR_MASK);

    for (int pdpt_i = 1; pdpt_i < 512; ++pdpt_i) {
        if (!(pdpt[pdpt_i] & PAGE_PRESENT) || is_huge(pdpt[pdpt_i])) continue;
        uint64_t* pd = table_ptr(pdpt[pdpt_i] & ADDR_MASK);

        for (int pd_i = 0; pd_i < 512; ++pd_i) {
            if (!(pd[pd_i] & PAGE_PRESENT) || is_huge(pd[pd_i])) continue;
            uint64_t* pt = table_ptr(pd[pd_i] & ADDR_MASK);

            for (int pt_i = 0; pt_i < 512; ++pt_i) {
                if (pt[pt_i] & PAGE_PRESENT) {
                    pmm::free_frame(pt[pt_i] & ADDR_MASK);
                }
            }
            pmm::free_frame(pd[pd_i] & ADDR_MASK);
        }
        pmm::free_frame(pdpt[pdpt_i] & ADDR_MASK);
    }

    pmm::free_frame(pml4[0] & ADDR_MASK);
    pmm::free_frame(pml4_phys);
}

bool self_test() {

    constexpr uintptr_t TEST_VIRT = 0x40000000;
    constexpr uint32_t  TEST_PATTERN = 0xDEADC0DEu;

    uintptr_t frame = pmm::alloc_frame();
    if (frame == 0) return false;

    if (!map_page(TEST_VIRT, frame)) {
        pmm::free_frame(frame);
        return false;
    }

    volatile uint32_t* test_ptr = reinterpret_cast<volatile uint32_t*>(TEST_VIRT);
    *test_ptr = TEST_PATTERN;
    bool readback_ok = (*test_ptr == TEST_PATTERN);

    bool unmap_ok = unmap_page(TEST_VIRT);
    pmm::free_frame(frame);

    return readback_ok && unmap_ok;
}

}
