#include "ata.hpp"
#include "../include/io.hpp"

namespace ata {

namespace {

constexpr uint16_t PORT_DATA        = 0x1F0;
constexpr uint16_t PORT_ERROR       = 0x1F1; 
constexpr uint16_t PORT_FEATURES    = 0x1F1; 
constexpr uint16_t PORT_SECTOR_COUNT = 0x1F2;
constexpr uint16_t PORT_LBA_LOW      = 0x1F3;
constexpr uint16_t PORT_LBA_MID      = 0x1F4;
constexpr uint16_t PORT_LBA_HIGH     = 0x1F5;
constexpr uint16_t PORT_DRIVE_HEAD   = 0x1F6;
constexpr uint16_t PORT_STATUS       = 0x1F7; 
constexpr uint16_t PORT_COMMAND      = 0x1F7; 
constexpr uint16_t PORT_ALT_STATUS   = 0x3F6;

constexpr uint8_t STATUS_ERR = 1 << 0;
constexpr uint8_t STATUS_DRQ = 1 << 3;
constexpr uint8_t STATUS_BSY = 1 << 7;

constexpr uint8_t CMD_READ_SECTORS  = 0x20;
constexpr uint8_t CMD_WRITE_SECTORS = 0x30;
constexpr uint8_t CMD_CACHE_FLUSH   = 0xE7;
constexpr uint8_t CMD_IDENTIFY      = 0xEC;

constexpr uint8_t DRIVE_MASTER_LBA = 0xE0;


void io_delay_400ns() {
    io::inb(PORT_ALT_STATUS);
    io::inb(PORT_ALT_STATUS);
    io::inb(PORT_ALT_STATUS);
    io::inb(PORT_ALT_STATUS);
}


bool wait_not_busy() {
    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        if ((io::inb(PORT_STATUS) & STATUS_BSY) == 0) return true;
    }
    return false;
}

bool wait_for_data() {
    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        uint8_t status = io::inb(PORT_STATUS);
        if (status & STATUS_ERR) return false;
        if (status & STATUS_DRQ) return true;
    }
    return false;
}

void select_drive_lba(uint32_t lba) {
    io::outb(PORT_DRIVE_HEAD, static_cast<uint8_t>(DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F)));
    io_delay_400ns();
}

} // namespace

bool init() {
    select_drive_lba(0);

    io::outb(PORT_SECTOR_COUNT, 0);
    io::outb(PORT_LBA_LOW, 0);
    io::outb(PORT_LBA_MID, 0);
    io::outb(PORT_LBA_HIGH, 0);
    io::outb(PORT_COMMAND, CMD_IDENTIFY);

    uint8_t status = io::inb(PORT_STATUS);
    if (status == 0) return false; 

    if (!wait_not_busy()) return false;

    if (io::inb(PORT_LBA_MID) != 0 || io::inb(PORT_LBA_HIGH) != 0) return false;

    if (!wait_for_data()) return false;

    for (int i = 0; i < 256; ++i) {
        io::inw(PORT_DATA);
    }

    return true;
}

bool read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (count == 0) return true;

    select_drive_lba(lba);
    if (!wait_not_busy()) return false;

    io::outb(PORT_SECTOR_COUNT, static_cast<uint8_t>(count));
    io::outb(PORT_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
    io::outb(PORT_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
    io::outb(PORT_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));
    io::outb(PORT_COMMAND, CMD_READ_SECTORS);

    uint16_t* words = reinterpret_cast<uint16_t*>(buffer);
    for (uint32_t sector = 0; sector < count; ++sector) {
        if (!wait_for_data()) return false;
        for (uint32_t i = 0; i < SECTOR_SIZE / 2; ++i) {
            words[sector * (SECTOR_SIZE / 2) + i] = io::inw(PORT_DATA);
        }
    }

    return true;
}

bool write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (count == 0) return true;

    select_drive_lba(lba);
    if (!wait_not_busy()) return false;

    io::outb(PORT_SECTOR_COUNT, static_cast<uint8_t>(count));
    io::outb(PORT_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
    io::outb(PORT_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
    io::outb(PORT_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));
    io::outb(PORT_COMMAND, CMD_WRITE_SECTORS);

    const uint16_t* words = reinterpret_cast<const uint16_t*>(buffer);
    for (uint32_t sector = 0; sector < count; ++sector) {
        if (!wait_for_data()) return false;
        for (uint32_t i = 0; i < SECTOR_SIZE / 2; ++i) {
            io::outw(PORT_DATA, words[sector * (SECTOR_SIZE / 2) + i]);
        }
    }

    io::outb(PORT_COMMAND, CMD_CACHE_FLUSH);
    if (!wait_not_busy()) return false;

    return true;
}

} 
