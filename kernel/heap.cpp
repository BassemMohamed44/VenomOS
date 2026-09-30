#include "heap.hpp"
#include "pmm.hpp"
#include "vga.hpp"

namespace heap {

namespace {

constexpr size_t ALIGNMENT = 16;
constexpr size_t MIN_SPLIT_REMAINDER = 32;
constexpr int    MAX_GROW_ATTEMPTS = 256;

constexpr uint32_t MAGIC_ALLOCATED = 0xC0FFEE11;
constexpr uint32_t MAGIC_FREED     = 0xDEADF4EE;

struct BlockHeader {
    size_t size;
    bool free;
    uint32_t magic;
    BlockHeader* next;

    uint64_t _reserved;
};

BlockHeader* free_list_head = nullptr;

uintptr_t heap_min = 0;
uintptr_t heap_max = 0;

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline uint8_t* data_ptr(BlockHeader* block) {
    return reinterpret_cast<uint8_t*>(block) + sizeof(BlockHeader);
}

inline bool adjacent(BlockHeader* first, BlockHeader* second) {
    return data_ptr(first) + first->size == reinterpret_cast<uint8_t*>(second);
}

void report_invalid_free(const char* reason) {
    vga::print_colored("heap: invalid kfree() rejected - ", vga::Color::LightRed);
    vga::print_colored(reason, vga::Color::LightRed);
    vga::print_colored("\n", vga::Color::LightRed);
}

inline bool in_heap_range(uintptr_t addr) {
    return heap_min != 0 && addr >= heap_min && addr < heap_max;
}

bool is_tracked_block(BlockHeader* block) {
    for (BlockHeader* b = free_list_head; b != nullptr; b = b->next) {
        if (b == block) return true;
    }
    return false;
}

void insert_free_block(BlockHeader* block) {
    block->free = true;
    block->magic = MAGIC_FREED;

    if (free_list_head == nullptr || block < free_list_head) {
        if (free_list_head != nullptr && adjacent(block, free_list_head)) {
            block->size += sizeof(BlockHeader) + free_list_head->size;
            block->next = free_list_head->next;
        } else {
            block->next = free_list_head;
        }
        free_list_head = block;
        return;
    }

    BlockHeader* prev = free_list_head;
    while (prev->next != nullptr && prev->next < block) {
        prev = prev->next;
    }

    block->next = prev->next;
    if (block->next != nullptr && adjacent(block, block->next)) {
        block->size += sizeof(BlockHeader) + block->next->size;
        block->next = block->next->next;
    }

    if (adjacent(prev, block) && prev->free) {
        prev->size += sizeof(BlockHeader) + block->size;
        prev->next = block->next;
    } else {
        prev->next = block;
    }
}

bool grow_heap() {
    uintptr_t frame = pmm::alloc_frame();
    if (frame == 0) return false;

    BlockHeader* block = reinterpret_cast<BlockHeader*>(frame);
    block->size = pmm::FRAME_SIZE - sizeof(BlockHeader);
    block->free = true;
    block->magic = MAGIC_FREED;
    block->next = nullptr;

    uintptr_t frame_end = frame + pmm::FRAME_SIZE;
    if (heap_min == 0 || frame < heap_min) heap_min = frame;
    if (frame_end > heap_max) heap_max = frame_end;

    insert_free_block(block);
    return true;
}

}

void init() {
    free_list_head = nullptr;

}

void* kmalloc(size_t size) {
    if (size == 0) return nullptr;
    size = align_up(size, ALIGNMENT);

    for (int attempt = 0; attempt <= MAX_GROW_ATTEMPTS; ++attempt) {
        for (BlockHeader* block = free_list_head; block != nullptr; block = block->next) {
            if (!block->free || block->size < size) continue;

            size_t remainder = block->size - size;
            if (remainder >= sizeof(BlockHeader) + MIN_SPLIT_REMAINDER) {

                BlockHeader* new_block = reinterpret_cast<BlockHeader*>(data_ptr(block) + size);
                new_block->size = remainder - sizeof(BlockHeader);
                new_block->free = true;
                new_block->magic = MAGIC_FREED;
                new_block->next = block->next;

                block->size = size;
                block->next = new_block;
            }

            block->free = false;
            block->magic = MAGIC_ALLOCATED;
            return data_ptr(block);
        }

        if (!grow_heap()) return nullptr;
    }

    return nullptr;
}

void kfree(void* ptr) {
    if (ptr == nullptr) return;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    if (addr % ALIGNMENT != 0) {
        report_invalid_free("misaligned pointer");
        return;
    }

    if (!in_heap_range(addr)) {
        report_invalid_free("pointer outside heap range");
        return;
    }

    if (addr < heap_min + sizeof(BlockHeader)) {
        report_invalid_free("pointer too low for a valid block header");
        return;
    }

    BlockHeader* block = reinterpret_cast<BlockHeader*>(addr - sizeof(BlockHeader));

    if (!is_tracked_block(block)) {
        report_invalid_free("pointer is not a tracked heap block");
        return;
    }

    if (block->magic == MAGIC_FREED) {
        report_invalid_free("double free detected");
        return;
    }

    if (block->magic != MAGIC_ALLOCATED || block->free) {
        report_invalid_free("corrupted block header");
        return;
    }

    block->free = true;
    block->magic = MAGIC_FREED;

    if (block->next != nullptr && block->next->free && adjacent(block, block->next)) {
        block->size += sizeof(BlockHeader) + block->next->size;
        block->next = block->next->next;
    }

    if (free_list_head != nullptr && free_list_head != block) {
        BlockHeader* prev = free_list_head;
        while (prev->next != nullptr && prev->next != block) {
            prev = prev->next;
        }
        if (prev->next == block && prev->free && adjacent(prev, block)) {
            prev->size += sizeof(BlockHeader) + block->size;
            prev->next = block->next;
        }
    }
}

size_t bytes_in_use() {
    size_t total = 0;
    for (BlockHeader* block = free_list_head; block != nullptr; block = block->next) {
        if (!block->free) total += block->size;
    }
    return total;
}

size_t bytes_free() {
    size_t total = 0;
    for (BlockHeader* block = free_list_head; block != nullptr; block = block->next) {
        if (block->free) total += block->size;
    }
    return total;
}

}
