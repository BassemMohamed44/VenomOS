#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace fs {

constexpr size_t MAX_FILENAME = 32;
constexpr int MAX_FILES = 64;
constexpr int MAX_EXTENTS = 4;

struct Extent {
    uint32_t start_lba;
    uint32_t block_count;
} __attribute__((packed));

struct FileEntry {
    char name[MAX_FILENAME];
    uint32_t size_bytes;
    uint8_t extent_count;
    uint8_t used;
    uint8_t reserved[2];
    Extent extents[MAX_EXTENTS];
} __attribute__((packed));

bool init();

bool exists(const char* name);

int64_t file_size(const char* name);

bool write(const char* name, const void* data, size_t size);

bool read(const char* name, void* buffer, size_t buffer_capacity, size_t* out_bytes_read);

bool remove(const char* name);

const FileEntry* entry_at(int index);

bool self_test();

}
