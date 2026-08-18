// ============================================================
// paging.cpp - VenomOS Phase 4 + Process Isolation phase
// ============================================================
#include "paging.hpp"
#include "pmm.hpp"

namespace paging {

namespace {

constexpr uint64_t PAGE_HUGE = 1ull << 7; // PS bit - marks a 2MB/1GB "huge" page entry
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ull; // bits 12-51: physical frame address

inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline void invalidate_page(uintptr_t virt_addr) {
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

inline uint64_t* table_ptr(uint64_t phys_addr) {
    // Valid because every page-table frame pmm::alloc_frame() hands
    // out lives within the first 1GB (that's all PMM manages), which
    // is identity-mapped in EVERY address space (see
    // create_address_space()): physical == virtual for these.
    return reinterpret_cast<uint64_t*>(phys_addr);
}

// Returns the physical address of the next-level table referenced by
// table[index], creating and zeroing a fresh one via the PMM first if
// that entry isn't present yet. Returns 0 on allocation failure, or
// if the entry is already a huge page (caller must check is_huge()
// before calling this, since walking "through" a huge page entry as
// if it were a table pointer would misinterpret its address bits).
//
// If the entry already exists (from an earlier, unrelated map_page()
// call that happened to share this same intermediate table - e.g.
// paging::self_test() and a later user-mode mapping can land in the
// same PML4/PDPT/PD slot if their addresses are close together), its
// flags are widened (OR'd) rather than left as-is. x86 requires every
// level of the translation hierarchy to have the USER bit set for a
// ring 3 access to succeed - not just the final page-table entry - so
// an intermediate table built before PAGE_USER was ever requested
// must still gain it once something does request it.
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

} // namespace

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

    if (is_huge(pdpt[pdpt_index])) return false; // a 1GB page already covers this - refuse

    uint64_t pd_phys = get_or_create_table(pdpt, pdpt_index, flags);
    if (pd_phys == 0) return false;
    uint64_t* pd = table_ptr(pd_phys);

    if (is_huge(pd[pd_index])) return false; // a boot-time 2MB huge page already covers this

    uint64_t pt_phys = get_or_create_table(pd, pd_index, flags);
    if (pt_phys == 0) return false;
    uint64_t* pt = table_ptr(pt_phys);

    pt[pt_index] = (phys_addr & ADDR_MASK) | (flags | PAGE_PRESENT);

    // Only worth flushing the TLB for this address if we're editing
    // whichever address space is actually loaded right now - editing
    // a not-currently-loaded process's page tables (the whole point
    // of map_page_in() taking an explicit PML4) can't have stale TLB
    // entries for addresses nothing has ever paged through yet.
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

    // The kernel's own PML4[0] entry points at the PDPT built back in
    // boot/stage2.asm, whose slot 0 is the 1GB identity-mapped huge-page
    // region every process needs to keep the kernel reachable. Borrow
    // (not copy) that specific PD chain into a brand new PDPT/PML4 pair.
    if (!(kernel_pml4[0] & PAGE_PRESENT)) return 0; // should never happen post-boot
    uint64_t* kernel_pdpt = table_ptr(kernel_pml4[0] & ADDR_MASK);
    uint64_t kernel_space_pdpt_entry = kernel_pdpt[0]; // the shared 1GB identity-map PD pointer

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

    new_pdpt[0] = kernel_space_pdpt_entry; // shared kernel identity map - borrowed, not owned
    new_pml4[0] = new_pdpt_frame | PAGE_PRESENT | PAGE_WRITABLE;

    return new_pml4_frame;
}

void destroy_address_space(uint64_t pml4_phys) {
    uint64_t* pml4 = table_ptr(pml4_phys);
    if (!(pml4[0] & PAGE_PRESENT)) { pmm::free_frame(pml4_phys); return; }

    uint64_t* pdpt = table_ptr(pml4[0] & ADDR_MASK);

    // Slot 0 is the SHARED kernel identity map, borrowed from the
    // kernel's own PDPT in create_address_space() - free everything
    // from slot 1 onward (process-private), but never slot 0 itself.
    for (int pdpt_i = 1; pdpt_i < 512; ++pdpt_i) {
        if (!(pdpt[pdpt_i] & PAGE_PRESENT) || is_huge(pdpt[pdpt_i])) continue;
        uint64_t* pd = table_ptr(pdpt[pdpt_i] & ADDR_MASK);

        for (int pd_i = 0; pd_i < 512; ++pd_i) {
            if (!(pd[pd_i] & PAGE_PRESENT) || is_huge(pd[pd_i])) continue;
            uint64_t* pt = table_ptr(pd[pd_i] & ADDR_MASK);

            for (int pt_i = 0; pt_i < 512; ++pt_i) {
                if (pt[pt_i] & PAGE_PRESENT) {
                    pmm::free_frame(pt[pt_i] & ADDR_MASK); // the data frame itself
                }
            }
            pmm::free_frame(pd[pd_i] & ADDR_MASK); // this PT
        }
        pmm::free_frame(pdpt[pdpt_i] & ADDR_MASK); // this PD
    }

    pmm::free_frame(pml4[0] & ADDR_MASK); // this process's own PDPT
    pmm::free_frame(pml4_phys);            // this process's own PML4
}

bool self_test() {
    // Deliberately chosen just past the 1GB boundary Stage 2's
    // boot-time huge pages stop at, so this genuinely exercises
    // map_page()'s table-creation path rather than landing on
    // memory that was already mapped before the kernel even started.
    constexpr uintptr_t TEST_VIRT = 0x40000000; // 1GB
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

} // namespace paging
