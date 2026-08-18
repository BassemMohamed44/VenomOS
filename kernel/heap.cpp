#include "heap.hpp"
#include "pmm.hpp"

namespace heap {

namespace {

constexpr size_t ALIGNMENT = 16;
constexpr size_t MIN_SPLIT_REMAINDER = 32;
constexpr int    MAX_GROW_ATTEMPTS = 256; 

struct BlockHeader {
    size_t size;        
    bool free;
    BlockHeader* next;   
};

BlockHeader* free_list_head = nullptr; 

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline uint8_t* data_ptr(BlockHeader* block) {
    return reinterpret_cast<uint8_t*>(block) + sizeof(BlockHeader);
}

inline bool adjacent(BlockHeader* first, BlockHeader* second) {
    return data_ptr(first) + first->size == reinterpret_cast<uint8_t*>(second);
}

void insert_free_block(BlockHeader* block) {
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
    block->next = nullptr;

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
                new_block->next = block->next;

                block->size = size;
                block->next = new_block;
            }

            block->free = false;
            return data_ptr(block);
        }

        if (!grow_heap()) return nullptr; 
    }

    return nullptr; 
}

void kfree(void* ptr) {
    if (ptr == nullptr) return;

    BlockHeader* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<uint8_t*>(ptr) - sizeof(BlockHeader));
    block->free = true;

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
