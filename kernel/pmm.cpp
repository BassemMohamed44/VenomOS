#include "pmm.hpp"

extern "C" uint8_t kernel_end;

namespace pmm {

namespace {

constexpr uintptr_t E820_COUNT_ADDR  = 0x8FF8;
constexpr uintptr_t E820_BUFFER_ADDR = 0x9000;
constexpr uint32_t  E820_TYPE_USABLE = 1;

struct E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_attributes;
} __attribute__((packed));

constexpr uintptr_t MANAGED_LIMIT = 0x40000000; 
constexpr uint64_t  MANAGED_FRAMES = MANAGED_LIMIT / FRAME_SIZE; 
constexpr uint64_t  BITMAP_BYTES = MANAGED_FRAMES / 8;         

uint8_t bitmap[BITMAP_BYTES];
uint64_t free_count = 0;

inline bool test_bit(uint64_t frame_index) {
    return (bitmap[frame_index / 8] >> (frame_index % 8)) & 1;
}

inline void set_bit(uint64_t frame_index) {
    bitmap[frame_index / 8] |= static_cast<uint8_t>(1u << (frame_index % 8));
}

inline void clear_bit(uint64_t frame_index) {
    bitmap[frame_index / 8] &= static_cast<uint8_t>(~(1u << (frame_index % 8)));
}

void mark_range_used(uintptr_t start, uintptr_t end) {
    if (end > MANAGED_LIMIT) end = MANAGED_LIMIT;
    if (start >= end) return;

    uint64_t first_frame = start / FRAME_SIZE;
    uint64_t last_frame  = (end + FRAME_SIZE - 1) / FRAME_SIZE;

    for (uint64_t frame = first_frame; frame < last_frame; ++frame) {
        if (test_bit(frame)) continue;
        set_bit(frame);
        --free_count;
    }
}

void mark_range_free(uintptr_t start, uintptr_t end) {
    if (end > MANAGED_LIMIT) end = MANAGED_LIMIT;
    if (start >= end) return;

    uint64_t first_frame = start / FRAME_SIZE;
    uint64_t last_frame  = (end + FRAME_SIZE - 1) / FRAME_SIZE;

    for (uint64_t frame = first_frame; frame < last_frame; ++frame) {
        if (!test_bit(frame)) continue;
        clear_bit(frame);
        ++free_count;
    }
}

}

void init() {
    const uintptr_t kernel_end_addr = reinterpret_cast<uintptr_t>(&kernel_end);

    for (uint64_t i = 0; i < BITMAP_BYTES; ++i) {
        bitmap[i] = 0xFF;
    }
    free_count = 0;

    const uint16_t entry_count = *reinterpret_cast<volatile uint16_t*>(E820_COUNT_ADDR);
    const E820Entry* entries = reinterpret_cast<const E820Entry*>(E820_BUFFER_ADDR);

    for (uint16_t i = 0; i < entry_count; ++i) {
        const E820Entry& entry = entries[i];
        if (entry.type != E820_TYPE_USABLE) continue;
        if (entry.base >= MANAGED_LIMIT) continue;

        uint64_t region_end = entry.base + entry.length;
        mark_range_free(static_cast<uintptr_t>(entry.base), static_cast<uintptr_t>(region_end));
    }

    mark_range_used(0x0, 0x100000);                   
    mark_range_used(0x100000, kernel_end_addr);        
}

uintptr_t alloc_frame() {
    for (uint64_t frame = 0; frame < MANAGED_FRAMES; ++frame) {
        if (!test_bit(frame)) {
            set_bit(frame);
            --free_count;
            return frame * FRAME_SIZE;
        }
    }
    return 0;
}

void free_frame(uintptr_t phys_addr) {
    if (phys_addr >= MANAGED_LIMIT) return;
    if (phys_addr % FRAME_SIZE != 0) return; 

    uint64_t frame = phys_addr / FRAME_SIZE;
    if (!test_bit(frame)) return; 

    clear_bit(frame);
    ++free_count;
}

uint64_t total_frames() { return MANAGED_FRAMES; }
uint64_t free_frames()  { return free_count; }
uint64_t used_frames()  { return MANAGED_FRAMES - free_count; }

}