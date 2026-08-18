// ============================================================
// paging.hpp - VenomOS Phase 4
//
// Stage 2 (boot/stage2.asm) already built a minimal set of page
// tables that identity-map the first 1GB of physical memory with
// 2MB "huge" pages - that was the minimum required just to enter
// Long Mode. This module builds on top of those same page tables
// (same PML4, found via CR3) to add genuine, dynamic 4KB mappings:
// new virtual-to-physical translations created and destroyed at
// runtime, allocating new page-table frames from the PMM as needed.
// ============================================================
#pragma once

#include "../include/stdint.hpp"

namespace paging {

constexpr uint64_t PAGE_PRESENT  = 1ull << 0;
constexpr uint64_t PAGE_WRITABLE = 1ull << 1;
constexpr uint64_t PAGE_USER     = 1ull << 2; // if unset, only ring 0 (CPL 0-2) can access this page

// Maps a single 4KB virtual page to a physical frame in the CURRENTLY
// LOADED address space (whatever CR3 is right now), creating any
// missing intermediate page-directory/page-table levels along the
// way (each backed by a fresh frame from pmm::alloc_frame()).
// Returns false if a required frame couldn't be allocated, if
// either address isn't 4KB-aligned, or if the target region is
// already covered by one of Stage 2's boot-time 2MB huge pages
// (splitting a huge page into 4KB pages is intentionally not
// supported - this module is for extending the map into address
// ranges nothing has claimed yet, not for altering the boot-time
// identity map).
bool map_page(uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE);

// Same as map_page(), but against an EXPLICITLY given address space
// (a PML4 physical address) instead of whatever CR3 currently is -
// needed to build a new process's page tables before ever switching
// to them. map_page() is just map_page_in() using the current CR3.
bool map_page_in(uint64_t pml4_phys, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE);

// Removes a mapping previously created by map_page(). Does nothing
// (and returns false) if the address wasn't mapped by map_page() in
// the first place.
bool unmap_page(uintptr_t virt_addr);

// Allocates a fresh PML4 for a new, isolated address space. Its
// slot 0 points at a fresh PDPT whose OWN slot 0 is shared with the
// kernel's existing PDPT slot 0 (the boot-time 1GB identity map, so
// kernel code/data/interrupt handlers/heap stay reachable no matter
// whose address space is loaded) - every other PDPT slot starts
// empty, ready for map_page_in() to build process-private mappings
// into (e.g. via process.cpp, at the 1GB+ range paging::self_test()
// already exercises). Returns 0 on allocation failure.
uint64_t create_address_space();

// Frees every frame private to the given address space: its PML4,
// its PDPT, and everything reachable through PDPT slots 1 and up
// (PDs, PTs, and the data frames they map) - but NOT the shared
// kernel PD reachable through PDPT slot 0, which create_address_space()
// borrowed rather than copied. Must only be called on a PML4
// returned by create_address_space(), and never while it's still the
// currently-loaded CR3.
void destroy_address_space(uint64_t pml4_phys);

// Exercises map_page()/unmap_page() against a real address (chosen
// just past the 1GB boundary, deliberately outside Stage 2's
// boot-time identity map) by mapping a fresh frame, writing a known
// pattern through the new virtual mapping, reading it back, and
// unmapping again. Returns true only if every step actually
// succeeded - this is a genuine functional test run at boot, not a
// hardcoded "OK" message.
bool self_test();

} // namespace paging
