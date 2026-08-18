#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace elf {

constexpr uint8_t ELFMAG0 = 0x7F;
constexpr uint8_t ELFMAG1 = 'E';
constexpr uint8_t ELFMAG2 = 'L';
constexpr uint8_t ELFMAG3 = 'F';

constexpr uint8_t ELFCLASS64 = 2;
constexpr uint8_t ELFDATA2LSB = 1; 

constexpr uint16_t ET_EXEC = 2;     
constexpr uint16_t EM_X86_64 = 62;

constexpr uint32_t PT_NULL = 0;    
constexpr uint32_t PT_LOAD = 1;    
constexpr uint32_t PT_GNU_STACK = 0x6474e551; 

constexpr uint32_t PF_EXEC  = 1 << 0;
constexpr uint32_t PF_WRITE = 1 << 1;
constexpr uint32_t PF_READ  = 1 << 2;
constexpr uint32_t PF_KNOWN_MASK = PF_EXEC | PF_WRITE | PF_READ; 

constexpr uint64_t USER_SPACE_MIN = 0x40000000ull;         
constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ull; 

constexpr uint16_t MAX_LOAD_SEGMENTS = 16;

struct [[gnu::packed]] Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;      
    uint64_t e_phoff;      
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;  
    uint16_t e_phnum;    
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct [[gnu::packed]] Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;   
    uint64_t p_vaddr;    
    uint64_t p_paddr;    
    uint64_t p_filesz;   
    uint64_t p_memsz;    
    uint64_t p_align;
};

bool validate(const uint8_t* file, size_t file_size,
              const Elf64_Ehdr** out_header, const Elf64_Phdr** out_phdrs,
              uint16_t* out_phnum, const char** out_error);

} 
