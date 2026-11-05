#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

// Read-only FAT32 driver (8.3 names, no long-file-name support, no writes).
// The volume is reached through a sector-reader callback so the same code can
// run on the ATA disk in the kernel and on a plain file in host-side tests.
namespace fat32 {

using ReadSectorsFn = bool (*)(uint32_t lba, uint32_t count, void* buffer);

struct DirEntryInfo {
    char name[13];      // "NAME.EXT", NUL-terminated
    bool is_dir;
    uint32_t size;
};
using ListFn = void (*)(const DirEntryInfo& entry, void* context);

// Parses and validates the boot sector of the volume starting at volume_lba.
// Rejects anything that is not a plain 512-byte-sector FAT32 volume.
bool mount(ReadSectorsFn reader, uint32_t volume_lba);
bool mounted();

// Path syntax: "/" separated 8.3 names, case-insensitive, leading "/" optional.
bool list(const char* path, ListFn callback, void* context);
int64_t file_size(const char* path);   // -1 if missing or a directory
bool read(const char* path, void* buffer, size_t capacity, size_t* out_bytes_read);

// Where the VenomOS disk image places the FAT32 volume (see Makefile).
constexpr uint32_t DISK_VOLUME_LBA = 32768;

// Mounts the volume on the ATA disk at DISK_VOLUME_LBA.
bool mount_disk();

bool self_test();

}
