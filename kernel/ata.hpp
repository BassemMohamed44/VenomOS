#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace ata {

constexpr size_t SECTOR_SIZE = 512;
bool init();

bool read_sectors(uint32_t lba, uint32_t count, void* buffer);

bool write_sectors(uint32_t lba, uint32_t count, const void* buffer);

} 
