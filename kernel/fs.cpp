#include "fs.hpp"
#include "ata.hpp"

namespace fs {

namespace {

constexpr uint32_t SUPERBLOCK_LBA = 256;

constexpr uint32_t FILE_TABLE_LBA = 257;
constexpr uint32_t FILE_TABLE_SECTORS = 6; 

constexpr uint32_t BITMAP_LBA = 263;
constexpr uint32_t BITMAP_SECTORS = 8;

constexpr uint32_t DATA_START_LBA = 271;


constexpr uint32_t TOTAL_DISK_SECTORS = 32768;
constexpr uint32_t TOTAL_DATA_BLOCKS = TOTAL_DISK_SECTORS - DATA_START_LBA;

static_assert(sizeof(FileEntry) == 48, "FileEntry layout drifted - FILE_TABLE_SECTORS math below assumes 48 bytes");
static_assert(MAX_FILES * sizeof(FileEntry) <= FILE_TABLE_SECTORS * ata::SECTOR_SIZE,
              "file table no longer fits in FILE_TABLE_SECTORS");
static_assert(TOTAL_DATA_BLOCKS <= BITMAP_SECTORS * ata::SECTOR_SIZE * 8,
              "data region no longer fits in what BITMAP_SECTORS can track");

struct Superblock {
    char magic[4]; 
    uint32_t version;
    uint32_t total_blocks;
    uint32_t file_table_lba;
    uint32_t file_table_sectors;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t data_start_lba;
} __attribute__((packed));

constexpr uint32_t VENOMFS_VERSION = 1;


FileEntry file_table[MAX_FILES];
uint8_t bitmap[BITMAP_SECTORS * ata::SECTOR_SIZE];

bool name_equals(const char* a, const char* b) {
    for (size_t i = 0; i < MAX_FILENAME; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return true; 
}

void copy_name(char* dest, const char* src) {
    size_t i = 0;
    for (; i < MAX_FILENAME - 1 && src[i] != '\0'; ++i) dest[i] = src[i];
    dest[i] = '\0';
}

bool bit_test(uint32_t block) {
    return (bitmap[block / 8] >> (block % 8)) & 1;
}

void bit_set(uint32_t block) {
    bitmap[block / 8] |= static_cast<uint8_t>(1u << (block % 8));
}

void bit_clear(uint32_t block) {
    bitmap[block / 8] &= static_cast<uint8_t>(~(1u << (block % 8)));
}

void save_file_table() {
    ata::write_sectors(FILE_TABLE_LBA, FILE_TABLE_SECTORS, file_table);
}

void save_bitmap() {
    ata::write_sectors(BITMAP_LBA, BITMAP_SECTORS, bitmap);
}

FileEntry* find(const char* name) {
    for (int i = 0; i < MAX_FILES; ++i) {
        if (file_table[i].used && name_equals(file_table[i].name, name)) {
            return &file_table[i];
        }
    }
    return nullptr;
}


void free_entry(FileEntry* entry) {
    uint32_t start_block = entry->start_lba - DATA_START_LBA;
    for (uint32_t i = 0; i < entry->block_count; ++i) {
        bit_clear(start_block + i);
    }
    entry->used = 0;
    entry->name[0] = '\0';
    entry->size_bytes = 0;
    entry->start_lba = 0;
    entry->block_count = 0;
}


bool find_free_run(uint32_t needed, uint32_t* out_start_block) {
    if (needed == 0) {
        *out_start_block = 0;
        return true;
    }

    uint32_t run_start = 0;
    uint32_t run_length = 0;

    for (uint32_t block = 0; block < TOTAL_DATA_BLOCKS; ++block) {
        if (!bit_test(block)) {
            if (run_length == 0) run_start = block;
            ++run_length;
            if (run_length == needed) {
                *out_start_block = run_start;
                return true;
            }
        } else {
            run_length = 0;
        }
    }

    return false;
}

void format() {
    Superblock sb = {};
    sb.magic[0] = 'V'; sb.magic[1] = 'N'; sb.magic[2] = 'F'; sb.magic[3] = 'S';
    sb.version = VENOMFS_VERSION;
    sb.total_blocks = TOTAL_DATA_BLOCKS;
    sb.file_table_lba = FILE_TABLE_LBA;
    sb.file_table_sectors = FILE_TABLE_SECTORS;
    sb.bitmap_lba = BITMAP_LBA;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_start_lba = DATA_START_LBA;

    uint8_t sb_sector[ata::SECTOR_SIZE] = {};
    __builtin_memcpy(sb_sector, &sb, sizeof(sb));
    ata::write_sectors(SUPERBLOCK_LBA, 1, sb_sector);

    for (int i = 0; i < MAX_FILES; ++i) {
        file_table[i] = {};
    }
    save_file_table();

    for (size_t i = 0; i < sizeof(bitmap); ++i) {
        bitmap[i] = 0;
    }
    save_bitmap();
}

} 

void init() {
    uint8_t sb_sector[ata::SECTOR_SIZE];
    ata::read_sectors(SUPERBLOCK_LBA, 1, sb_sector);

    Superblock sb;
    __builtin_memcpy(&sb, sb_sector, sizeof(sb));

    bool valid_magic = (sb.magic[0] == 'V' && sb.magic[1] == 'N' &&
                         sb.magic[2] == 'F' && sb.magic[3] == 'S');

    if (!valid_magic) {
        format();
        return;
    }

    ata::read_sectors(FILE_TABLE_LBA, FILE_TABLE_SECTORS, file_table);
    ata::read_sectors(BITMAP_LBA, BITMAP_SECTORS, bitmap);
}

bool exists(const char* name) {
    return find(name) != nullptr;
}

int64_t file_size(const char* name) {
    FileEntry* entry = find(name);
    if (entry == nullptr) return -1;
    return static_cast<int64_t>(entry->size_bytes);
}

bool write(const char* name, const void* data, size_t size) {
    size_t name_len = 0;
    while (name[name_len] != '\0') {
        if (++name_len >= MAX_FILENAME) return false; // too long
    }

    FileEntry* existing = find(name);
    if (existing != nullptr) {
        free_entry(existing);
    }

    FileEntry* slot = existing;
    if (slot == nullptr) {
        for (int i = 0; i < MAX_FILES; ++i) {
            if (!file_table[i].used) { slot = &file_table[i]; break; }
        }
    }
    if (slot == nullptr) {
        save_bitmap();
        save_file_table();
        return false;
    }

    uint32_t blocks_needed = static_cast<uint32_t>((size + ata::SECTOR_SIZE - 1) / ata::SECTOR_SIZE);
    uint32_t start_block = 0;
    if (blocks_needed > 0 && !find_free_run(blocks_needed, &start_block)) {
        save_bitmap();
        save_file_table();
        return false;
    }

    for (uint32_t i = 0; i < blocks_needed; ++i) {
        bit_set(start_block + i);
    }

    if (blocks_needed > 0) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
        uint8_t sector_buf[ata::SECTOR_SIZE];
        for (uint32_t i = 0; i < blocks_needed; ++i) {
            size_t offset = static_cast<size_t>(i) * ata::SECTOR_SIZE;
            size_t remaining = size - offset;
            if (remaining >= ata::SECTOR_SIZE) {
                ata::write_sectors(DATA_START_LBA + start_block + i, 1, src + offset);
            } else {
                for (size_t b = 0; b < ata::SECTOR_SIZE; ++b) {
                    sector_buf[b] = (b < remaining) ? src[offset + b] : 0;
                }
                ata::write_sectors(DATA_START_LBA + start_block + i, 1, sector_buf);
            }
        }
    }

    copy_name(slot->name, name);
    slot->size_bytes = static_cast<uint32_t>(size);
    slot->start_lba = DATA_START_LBA + start_block;
    slot->block_count = blocks_needed;
    slot->used = 1;

    save_file_table();
    save_bitmap();
    return true;
}

bool read(const char* name, void* buffer, size_t buffer_capacity, size_t* out_bytes_read) {
    FileEntry* entry = find(name);
    if (entry == nullptr) return false;

    size_t to_read = entry->size_bytes;
    if (to_read > buffer_capacity) to_read = buffer_capacity;

    if (to_read > 0) {
        uint8_t sector_buf[ata::SECTOR_SIZE];
        uint8_t* dest = reinterpret_cast<uint8_t*>(buffer);
        size_t remaining = to_read;
        uint32_t lba = entry->start_lba;

        while (remaining > 0) {
            ata::read_sectors(lba, 1, sector_buf);
            size_t chunk = remaining < ata::SECTOR_SIZE ? remaining : ata::SECTOR_SIZE;
            for (size_t b = 0; b < chunk; ++b) dest[b] = sector_buf[b];
            dest += chunk;
            remaining -= chunk;
            ++lba;
        }
    }

    if (out_bytes_read != nullptr) *out_bytes_read = to_read;
    return true;
}

bool remove(const char* name) {
    FileEntry* entry = find(name);
    if (entry == nullptr) return false;

    free_entry(entry);
    save_file_table();
    save_bitmap();
    return true;
}

const FileEntry* entry_at(int index) {
    if (index < 0 || index >= MAX_FILES) return nullptr;
    return &file_table[index];
}

} 
