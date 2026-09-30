#include "fs.hpp"
#include "ata.hpp"

namespace fs {

namespace {

constexpr uint32_t SUPERBLOCK_LBA = 256;

constexpr uint32_t FILE_TABLE_LBA = 257;
constexpr uint32_t FILE_TABLE_SECTORS = 9;

constexpr uint32_t BITMAP_LBA = 266;
constexpr uint32_t BITMAP_SECTORS = 8;

constexpr uint32_t DATA_START_LBA = 274;

constexpr uint32_t TOTAL_DISK_SECTORS = 32768;
constexpr uint32_t TOTAL_DATA_BLOCKS = TOTAL_DISK_SECTORS - DATA_START_LBA;

static_assert(sizeof(FileEntry) == 72, "FileEntry layout drifted - FILE_TABLE_SECTORS math below assumes 72 bytes");
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

constexpr uint32_t VENOMFS_VERSION = 2;

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

bool save_file_table() {
    return ata::write_sectors(FILE_TABLE_LBA, FILE_TABLE_SECTORS, file_table);
}

bool save_bitmap() {
    return ata::write_sectors(BITMAP_LBA, BITMAP_SECTORS, bitmap);
}

void rebuild_bitmap_from_file_table() {
    for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap[i] = 0;
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!file_table[i].used) continue;
        for (int e = 0; e < file_table[i].extent_count; ++e) {
            uint32_t start_block = file_table[i].extents[e].start_lba - DATA_START_LBA;
            for (uint32_t b = 0; b < file_table[i].extents[e].block_count; ++b) {
                bit_set(start_block + b);
            }
        }
    }
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
    for (int e = 0; e < entry->extent_count; ++e) {
        uint32_t start_block = entry->extents[e].start_lba - DATA_START_LBA;
        for (uint32_t i = 0; i < entry->extents[e].block_count; ++i) {
            bit_clear(start_block + i);
        }
    }
    entry->used = 0;
    entry->name[0] = '\0';
    entry->size_bytes = 0;
    entry->extent_count = 0;
    for (int e = 0; e < MAX_EXTENTS; ++e) {
        entry->extents[e].start_lba = 0;
        entry->extents[e].block_count = 0;
    }
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

int find_free_extents(uint32_t needed, Extent* out_extents) {
    int count = 0;
    uint32_t remaining = needed;
    uint32_t block = 0;

    while (block < TOTAL_DATA_BLOCKS && remaining > 0 && count < MAX_EXTENTS) {
        if (bit_test(block)) {
            ++block;
            continue;
        }
        uint32_t run_start = block;
        uint32_t run_length = 0;
        while (block < TOTAL_DATA_BLOCKS && !bit_test(block) && run_length < remaining) {
            ++run_length;
            ++block;
        }
        out_extents[count].start_lba = DATA_START_LBA + run_start;
        out_extents[count].block_count = run_length;
        ++count;
        remaining -= run_length;
    }

    if (remaining > 0) return 0;
    return count;
}

bool format() {
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

    if (!ata::write_sectors(SUPERBLOCK_LBA, 1, sb_sector)) return false;

    for (int i = 0; i < MAX_FILES; ++i) {
        file_table[i] = {};
    }
    if (!save_file_table()) return false;

    for (size_t i = 0; i < sizeof(bitmap); ++i) {
        bitmap[i] = 0;
    }
    if (!save_bitmap()) return false;

    return true;
}

}

bool init() {
    uint8_t sb_sector[ata::SECTOR_SIZE];

    if (!ata::read_sectors(SUPERBLOCK_LBA, 1, sb_sector)) return false;

    Superblock sb;
    __builtin_memcpy(&sb, sb_sector, sizeof(sb));

    bool valid_magic = (sb.magic[0] == 'V' && sb.magic[1] == 'N' &&
                         sb.magic[2] == 'F' && sb.magic[3] == 'S');

    if (!valid_magic) {

        return format();
    }

    if (!ata::read_sectors(FILE_TABLE_LBA, FILE_TABLE_SECTORS, file_table)) return false;

    rebuild_bitmap_from_file_table();
    if (!save_bitmap()) return false;

    return true;
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
        if (++name_len >= MAX_FILENAME) return false;
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

    Extent extents[MAX_EXTENTS] = {};
    int extent_count = 0;

    if (blocks_needed > 0) {
        uint32_t single_start = 0;
        if (find_free_run(blocks_needed, &single_start)) {
            extents[0].start_lba = DATA_START_LBA + single_start;
            extents[0].block_count = blocks_needed;
            extent_count = 1;
        } else {
            extent_count = find_free_extents(blocks_needed, extents);
            if (extent_count == 0) {
                save_bitmap();
                save_file_table();
                return false;
            }
        }

        for (int e = 0; e < extent_count; ++e) {
            uint32_t start_block = extents[e].start_lba - DATA_START_LBA;
            for (uint32_t i = 0; i < extents[e].block_count; ++i) {
                bit_set(start_block + i);
            }
        }
    }

    bool data_write_ok = true;
    if (blocks_needed > 0) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
        uint8_t sector_buf[ata::SECTOR_SIZE];
        size_t offset = 0;

        for (int e = 0; e < extent_count && data_write_ok; ++e) {
            for (uint32_t i = 0; i < extents[e].block_count; ++i) {
                size_t remaining = size - offset;

                bool sector_ok;
                if (remaining >= ata::SECTOR_SIZE) {
                    sector_ok = ata::write_sectors(extents[e].start_lba + i, 1, src + offset);
                } else {
                    for (size_t b = 0; b < ata::SECTOR_SIZE; ++b) {
                        sector_buf[b] = (b < remaining) ? src[offset + b] : 0;
                    }
                    sector_ok = ata::write_sectors(extents[e].start_lba + i, 1, sector_buf);
                }

                if (!sector_ok) {
                    data_write_ok = false;
                    break;
                }
                offset += ata::SECTOR_SIZE;
            }
        }
    }

    if (!data_write_ok) {
        for (int e = 0; e < extent_count; ++e) {
            uint32_t start_block = extents[e].start_lba - DATA_START_LBA;
            for (uint32_t i = 0; i < extents[e].block_count; ++i) {
                bit_clear(start_block + i);
            }
        }
        save_bitmap();
        save_file_table();
        return false;
    }

    copy_name(slot->name, name);
    slot->size_bytes = static_cast<uint32_t>(size);
    slot->extent_count = static_cast<uint8_t>(extent_count);
    for (int e = 0; e < MAX_EXTENTS; ++e) {
        if (e < extent_count) {
            slot->extents[e] = extents[e];
        } else {
            slot->extents[e].start_lba = 0;
            slot->extents[e].block_count = 0;
        }
    }
    slot->used = 1;

    bool table_ok = save_file_table();
    bool bitmap_ok = save_bitmap();
    if (!table_ok || !bitmap_ok) {
        for (int e = 0; e < extent_count; ++e) {
            uint32_t start_block = extents[e].start_lba - DATA_START_LBA;
            for (uint32_t i = 0; i < extents[e].block_count; ++i) {
                bit_clear(start_block + i);
            }
        }
        slot->used = 0;
        slot->name[0] = '\0';
        slot->size_bytes = 0;
        slot->extent_count = 0;
        for (int e = 0; e < MAX_EXTENTS; ++e) {
            slot->extents[e].start_lba = 0;
            slot->extents[e].block_count = 0;
        }
        save_file_table();
        save_bitmap();
        return false;
    }

    return true;
}

bool read(const char* name, void* buffer, size_t buffer_capacity, size_t* out_bytes_read) {
    FileEntry* entry = find(name);
    if (entry == nullptr) return false;

    size_t to_read = entry->size_bytes;
    if (to_read > buffer_capacity) to_read = buffer_capacity;

    size_t bytes_read = 0;
    uint8_t* dest = reinterpret_cast<uint8_t*>(buffer);

    if (to_read > 0) {
        uint8_t sector_buf[ata::SECTOR_SIZE];

        for (int e = 0; e < entry->extent_count && bytes_read < to_read; ++e) {
            for (uint32_t i = 0; i < entry->extents[e].block_count && bytes_read < to_read; ++i) {
                if (!ata::read_sectors(entry->extents[e].start_lba + i, 1, sector_buf)) {
                    if (out_bytes_read != nullptr) *out_bytes_read = bytes_read;
                    return false;
                }
                size_t remaining = to_read - bytes_read;
                size_t chunk = remaining < ata::SECTOR_SIZE ? remaining : ata::SECTOR_SIZE;
                for (size_t b = 0; b < chunk; ++b) dest[b] = sector_buf[b];
                dest += chunk;
                bytes_read += chunk;
            }
        }
    }

    if (out_bytes_read != nullptr) *out_bytes_read = bytes_read;
    return true;
}

bool remove(const char* name) {
    FileEntry* entry = find(name);
    if (entry == nullptr) return false;

    FileEntry backup = *entry;
    free_entry(entry);

    bool table_ok = save_file_table();
    bool bitmap_ok = save_bitmap();
    if (!table_ok || !bitmap_ok) {
        *entry = backup;
        for (int e = 0; e < backup.extent_count; ++e) {
            uint32_t start_block = backup.extents[e].start_lba - DATA_START_LBA;
            for (uint32_t i = 0; i < backup.extents[e].block_count; ++i) {
                bit_set(start_block + i);
            }
        }
        return false;
    }

    return true;
}

const FileEntry* entry_at(int index) {
    if (index < 0 || index >= MAX_FILES) return nullptr;
    return &file_table[index];
}

bool self_test() {

    const char* name_ok       = "__fs_selftest_ok__";
    const char* name_data_fail = "__fs_selftest_dwfail__";
    const char* name_meta_fail = "__fs_selftest_mdfail__";

    if (exists(name_ok) || exists(name_data_fail) || exists(name_meta_fail)) {
        return false;
    }

    bool ok = true;

    {
        uint8_t data[700];
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i);

        if (!write(name_ok, data, sizeof(data))) ok = false;
        if (!exists(name_ok)) ok = false;

        uint8_t readback[700] = {};
        size_t bytes_read = 0;
        if (!read(name_ok, readback, sizeof(readback), &bytes_read)) ok = false;
        if (bytes_read != sizeof(data)) ok = false;
        for (size_t i = 0; i < sizeof(data) && ok; ++i) {
            if (readback[i] != data[i]) ok = false;
        }

        if (!remove(name_ok)) ok = false;
        if (exists(name_ok)) ok = false;
    }

    {
        uint8_t data[700];
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(0xA5);

        ata::debug_fail_next(1);
        bool result = write(name_data_fail, data, sizeof(data));

        if (result) ok = false;
        if (exists(name_data_fail)) ok = false;
    }

    {
        uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};

        ata::debug_fail_next(2);
        bool result = write(name_meta_fail, data, sizeof(data));

        if (result) ok = false;
        if (exists(name_meta_fail)) ok = false;
    }

    {
        uint8_t data[10] = {9,9,9,9,9,9,9,9,9,9};
        if (!write(name_ok, data, sizeof(data))) ok = false;

        ata::debug_fail_next(0);
        bool result = remove(name_ok);

        if (result) ok = false;
        if (!exists(name_ok)) ok = false;

        if (!remove(name_ok)) ok = false;
    }

    {
        const char* name_frag = "__fs_selftest_fragmented__";

        uint8_t bitmap_backup[sizeof(bitmap)];
        for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap_backup[i] = bitmap[i];

        for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap[i] = 0xFF;
        bit_clear(10);
        bit_clear(20);

        uint8_t data[1024];
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(0x77);

        bool frag_write_ok = write(name_frag, data, sizeof(data));
        FileEntry* frag_entry = find(name_frag);

        if (!frag_write_ok || frag_entry == nullptr) {
            ok = false;
        } else {
            if (frag_entry->extent_count != 2) ok = false;

            uint8_t readback[1024] = {};
            size_t bytes_read = 0;
            if (!read(name_frag, readback, sizeof(readback), &bytes_read)) ok = false;
            if (bytes_read != sizeof(data)) ok = false;
            for (size_t i = 0; i < sizeof(data) && ok; ++i) {
                if (readback[i] != data[i]) ok = false;
            }
        }

        if (exists(name_frag)) remove(name_frag);

        for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap[i] = bitmap_backup[i];
        rebuild_bitmap_from_file_table();
        save_bitmap();
    }

    if (exists(name_ok)) remove(name_ok);
    if (exists(name_data_fail)) remove(name_data_fail);
    if (exists(name_meta_fail)) remove(name_meta_fail);

    return ok;
}

}
