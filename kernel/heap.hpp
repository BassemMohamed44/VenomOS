#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace heap {

void init();

void* kmalloc(size_t size);
void  kfree(void* ptr);

size_t bytes_in_use();
size_t bytes_free();

} 
