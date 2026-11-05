#include "fat32.hpp"
#include "ata.hpp"

namespace fat32 {

namespace {

constexpr uint32_t SECTOR = 512;
constexpr uint32_t FAT_EOC_MIN = 0x0FFFFFF8;
constexpr uint32_t FAT_BAD = 0x0FFFFFF7;
constexpr uint32_t FAT_MASK = 0x0FFFFFFF;
constexpr uint8_t ATTR_DIRECTORY = 0x10;
constexpr uint8_t ATTR_VOLUME_ID = 0x08;
constexpr uint8_t ATTR_LFN = 0x0F;
constexpr int MAX_PATH_PARTS = 8;

struct Volume {
    ReadSectorsFn reader = nullptr;
    uint32_t volume_lba = 0;
    uint32_t sectors_per_cluster = 0;
    uint32_t reserved_sectors = 0;
    uint32_t fat_sectors = 0;
    uint32_t data_start = 0;        // sector offset of cluster 2, relative to the volume
    uint32_t root_cluster = 0;
    uint32_t cluster_count = 0;
    bool ok = false;
};

Volume vol;
uint8_t sector_buf[SECTOR];
uint8_t fat_cache[SECTOR];
uint32_t fat_cache_sector = 0xFFFFFFFFu;

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool read_volume_sector(uint32_t sector, void* buf) {
    return vol.reader(vol.volume_lba + sector, 1, buf);
}

bool valid_cluster(uint32_t c) { return c >= 2 && c < vol.cluster_count + 2; }

uint32_t cluster_sector(uint32_t c) { return vol.data_start + (c - 2) * vol.sectors_per_cluster; }

// Returns the next cluster in the chain. 0 = end of chain, 0xFFFFFFFF = error
// (bad, free or out-of-range link).
uint32_t next_cluster(uint32_t c) {
    const uint32_t byte_off = c * 4;
    const uint32_t sector = vol.reserved_sectors + byte_off / SECTOR;
    if (byte_off / SECTOR >= vol.fat_sectors) return 0xFFFFFFFFu;
    if (fat_cache_sector != sector) {
        if (!read_volume_sector(sector, fat_cache)) { fat_cache_sector = 0xFFFFFFFFu; return 0xFFFFFFFFu; }
        fat_cache_sector = sector;
    }
    const uint32_t v = rd32(&fat_cache[byte_off % SECTOR]) & FAT_MASK;
    if (v >= FAT_EOC_MIN) return 0;
    // A cluster that links to itself is corrupt. Longer cycles cannot hang the
    // driver either: every walk is bounded by the file size / cluster count.
    if (v == FAT_BAD || !valid_cluster(v) || v == c) return 0xFFFFFFFFu;
    return v;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

// "name.ext" -> 11 space-padded upper-case bytes. False if not a valid 8.3 name.
bool to_short_name(const char* part, size_t len, uint8_t out[11]) {
    for (int i = 0; i < 11; ++i) out[i] = ' ';
    size_t i = 0, n = 0;
    if (len == 0) return false;
    while (i < len && part[i] != '.') {
        if (n >= 8) return false;
        out[n++] = static_cast<uint8_t>(upper(part[i++]));
    }
    if (n == 0) return false;
    if (i < len) {
        ++i;
        size_t e = 0;
        if (i >= len) return false;
        while (i < len) {
            if (part[i] == '.' || e >= 3) return false;
            out[8 + e++] = static_cast<uint8_t>(upper(part[i++]));
        }
    }
    return true;
}

struct Found {
    uint32_t first_cluster;
    uint32_t size;
    bool is_dir;
};

// Walks every 32-byte entry of the directory starting at `dir_cluster`.
// `visit` returns true to stop. Returns false only on an I/O or chain error.
template <typename Visit>
bool walk_dir(uint32_t dir_cluster, Visit visit) {
    uint32_t cluster = dir_cluster;
    uint32_t steps = 0;
    while (cluster != 0) {
        if (!valid_cluster(cluster) || ++steps > vol.cluster_count) return false;
        for (uint32_t s = 0; s < vol.sectors_per_cluster; ++s) {
            if (!read_volume_sector(cluster_sector(cluster) + s, sector_buf)) return false;
            for (uint32_t off = 0; off < SECTOR; off += 32) {
                const uint8_t* e = &sector_buf[off];
                if (e[0] == 0x00) return true;          // end of directory
                if (e[0] == 0xE5) continue;             // deleted
                const uint8_t attr = e[11];
                if (attr == ATTR_LFN || (attr & ATTR_VOLUME_ID)) continue;
                if (visit(e)) return true;
            }
        }
        cluster = next_cluster(cluster);
        if (cluster == 0xFFFFFFFFu) return false;
    }
    return true;
}

inline uint32_t entry_cluster(const uint8_t* e) {
    return (static_cast<uint32_t>(rd16(e + 20)) << 16) | rd16(e + 26);
}

// Resolves `path` to its directory entry. The root has no entry: returns
// root=true with first_cluster set to the root cluster.
bool resolve(const char* path, Found* out) {
    if (!vol.ok || path == nullptr) return false;

    while (*path == '/') ++path;

    Found cur = { vol.root_cluster, 0, true };
    while (*path != '\0') {
        size_t len = 0;
        while (path[len] != '\0' && path[len] != '/') ++len;
        uint8_t want[11];
        if (!to_short_name(path, len, want)) return false;
        path += len;
        while (*path == '/') ++path;

        if (!cur.is_dir) return false;       // a file in the middle of the path

        bool found = false;
        Found next = {};
        const bool walked = walk_dir(cur.first_cluster, [&](const uint8_t* e) {
            for (int i = 0; i < 11; ++i) if (e[i] != want[i]) return false;
            next.first_cluster = entry_cluster(e);
            next.size = rd32(e + 28);
            next.is_dir = (e[11] & ATTR_DIRECTORY) != 0;
            found = true;
            return true;
        });
        if (!walked || !found) return false;
        // ".." pointing at the root is stored as cluster 0
        if (next.is_dir && next.first_cluster == 0) next.first_cluster = vol.root_cluster;
        cur = next;
    }
    *out = cur;
    return true;
}

}  // namespace

bool mount(ReadSectorsFn reader, uint32_t volume_lba) {
    vol = Volume{};
    fat_cache_sector = 0xFFFFFFFFu;
    if (reader == nullptr) return false;

    uint8_t bs[SECTOR];
    if (!reader(volume_lba, 1, bs)) return false;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return false;

    const uint16_t bytes_per_sector = rd16(&bs[11]);
    const uint8_t spc = bs[13];
    const uint16_t reserved = rd16(&bs[14]);
    const uint8_t num_fats = bs[16];
    const uint16_t root_entries = rd16(&bs[17]);
    const uint16_t total16 = rd16(&bs[19]);
    const uint16_t fat16 = rd16(&bs[22]);
    const uint32_t total32 = rd32(&bs[32]);
    const uint32_t fat32_sectors = rd32(&bs[36]);
    const uint16_t fs_version = rd16(&bs[42]);
    const uint32_t root_cluster = rd32(&bs[44]);

    if (bytes_per_sector != SECTOR) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;           // power of two, 1..128
    if (reserved == 0 || num_fats == 0) return false;
    if (root_entries != 0 || total16 != 0 || fat16 != 0) return false;   // those are FAT12/16 fields
    if (fs_version != 0) return false;
    if (fat32_sectors == 0 || total32 == 0) return false;

    const uint64_t fats_total = static_cast<uint64_t>(num_fats) * fat32_sectors;
    const uint64_t data_start = static_cast<uint64_t>(reserved) + fats_total;
    if (data_start >= total32) return false;
    const uint32_t clusters = static_cast<uint32_t>((total32 - data_start) / spc);
    if (clusters < 65525) return false;                              // that makes it FAT12/16
    // The FAT must be able to describe every cluster.
    if (static_cast<uint64_t>(fat32_sectors) * (SECTOR / 4) < static_cast<uint64_t>(clusters) + 2) return false;
    if (root_cluster < 2 || root_cluster >= clusters + 2) return false;

    vol.reader = reader;
    vol.volume_lba = volume_lba;
    vol.sectors_per_cluster = spc;
    vol.reserved_sectors = reserved;
    vol.fat_sectors = fat32_sectors;
    vol.data_start = static_cast<uint32_t>(data_start);
    vol.root_cluster = root_cluster;
    vol.cluster_count = clusters;
    vol.ok = true;
    return true;
}

bool mounted() { return vol.ok; }

bool list(const char* path, ListFn callback, void* context) {
    Found dir;
    if (!resolve(path, &dir) || !dir.is_dir) return false;
    return walk_dir(dir.first_cluster, [&](const uint8_t* e) {
        DirEntryInfo info;
        int n = 0;
        for (int i = 0; i < 8 && e[i] != ' '; ++i) info.name[n++] = static_cast<char>(e[i]);
        if (e[8] != ' ') {
            info.name[n++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; ++i) info.name[n++] = static_cast<char>(e[i]);
        }
        info.name[n] = '\0';
        info.is_dir = (e[11] & ATTR_DIRECTORY) != 0;
        info.size = info.is_dir ? 0 : rd32(e + 28);
        callback(info, context);
        return false;
    });
}

int64_t file_size(const char* path) {
    Found f;
    if (!resolve(path, &f) || f.is_dir) return -1;
    return static_cast<int64_t>(f.size);
}

bool read(const char* path, void* buffer, size_t capacity, size_t* out_bytes_read) {
    if (out_bytes_read != nullptr) *out_bytes_read = 0;
    Found f;
    if (!resolve(path, &f) || f.is_dir) return false;

    size_t to_read = f.size;
    if (to_read > capacity) to_read = capacity;

    uint8_t* dest = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    uint32_t cluster = f.first_cluster;
    uint32_t steps = 0;
    while (done < to_read) {
        if (!valid_cluster(cluster) || ++steps > vol.cluster_count) return false;
        for (uint32_t s = 0; s < vol.sectors_per_cluster && done < to_read; ++s) {
            if (!read_volume_sector(cluster_sector(cluster) + s, sector_buf)) {
                if (out_bytes_read != nullptr) *out_bytes_read = done;
                return false;
            }
            size_t chunk = to_read - done;
            if (chunk > SECTOR) chunk = SECTOR;
            for (size_t b = 0; b < chunk; ++b) dest[done + b] = sector_buf[b];
            done += chunk;
        }
        if (done >= to_read) break;
        cluster = next_cluster(cluster);
        if (cluster == 0 || cluster == 0xFFFFFFFFu) {       // chain ended before the file did
            if (out_bytes_read != nullptr) *out_bytes_read = done;
            return false;
        }
    }
    if (out_bytes_read != nullptr) *out_bytes_read = done;
    return true;
}

bool mount_disk() {
    return mount(&ata::read_sectors, DISK_VOLUME_LBA);
}

bool self_test() {
    if (!vol.ok) return false;
    static const char expected[] = "Hello from a FAT32 volume!\n";
    char buf[64];
    size_t got = 0;
    if (!read("FATTEST.TXT", buf, sizeof(buf), &got)) return false;
    if (got != sizeof(expected) - 1) return false;
    for (size_t i = 0; i < got; ++i) if (buf[i] != expected[i]) return false;
    if (file_size("/fattest.txt") != static_cast<int64_t>(sizeof(expected) - 1)) return false;   // case-insensitive
    if (file_size("NOPE.TXT") != -1) return false;
    if (read("FATTEST.TXT/X", buf, sizeof(buf), &got)) return false;                               // file used as a directory
    return true;
}

}  // namespace fat32
