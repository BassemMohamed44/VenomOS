#include "elf.hpp"

namespace elf {

namespace {

constexpr uint64_t U64_MAX = ~0ull;
constexpr uint64_t PAGE_SIZE = 0x1000ull;
constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;

inline bool add_overflows(uint64_t a, uint64_t b) {
    return b > (U64_MAX - a);
}

inline bool is_power_of_two(uint64_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

} 
bool validate(const uint8_t* file, size_t file_size,
              const Elf64_Ehdr** out_header, const Elf64_Phdr** out_phdrs,
              uint16_t* out_phnum, const char** out_error) {
    if (file_size < sizeof(Elf64_Ehdr)) {
        *out_error = "File too small to contain an ELF header.";
        return false;
    }

    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(file);

    if (header->e_ident[0] != ELFMAG0 || header->e_ident[1] != ELFMAG1 ||
        header->e_ident[2] != ELFMAG2 || header->e_ident[3] != ELFMAG3) {
        *out_error = "Bad ELF magic - not an ELF file.";
        return false;
    }
    if (header->e_ident[4] != ELFCLASS64) {
        *out_error = "Not a 64-bit ELF file (ELFCLASS64 required).";
        return false;
    }
    if (header->e_ident[5] != ELFDATA2LSB) {
        *out_error = "Not little-endian (ELFDATA2LSB required).";
        return false;
    }
    if (header->e_type != ET_EXEC) {
        *out_error = "Not a static executable (ET_EXEC required - no PIE/shared objects).";
        return false;
    }
    if (header->e_machine != EM_X86_64) {
        *out_error = "Not an x86-64 executable.";
        return false;
    }

    if (header->e_phnum == 0) {
        *out_error = "ELF file has no program headers.";
        return false;
    }
    if (header->e_phnum > MAX_LOAD_SEGMENTS) {
        *out_error = "Too many program headers.";
        return false;
    }
    if (header->e_phentsize != sizeof(Elf64_Phdr)) {
        *out_error = "Unexpected program header entry size.";
        return false;
    }

    uint64_t phdr_table_bytes = static_cast<uint64_t>(header->e_phnum) * sizeof(Elf64_Phdr);
    if (add_overflows(header->e_phoff, phdr_table_bytes)) {
        *out_error = "Program header table offset overflows.";
        return false;
    }
    if (header->e_phoff + phdr_table_bytes > file_size) {
        *out_error = "Program header table runs past end of file.";
        return false;
    }

    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(file + header->e_phoff);

    uint64_t load_start[MAX_LOAD_SEGMENTS];
    uint64_t load_end[MAX_LOAD_SEGMENTS];
    int load_count = 0;

    for (uint16_t i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr& phdr = phdrs[i];

        if (phdr.p_type == PT_NULL || phdr.p_type == PT_GNU_STACK) {
            continue;
        }
        if (phdr.p_type != PT_LOAD) {
            *out_error = "Unsupported program header type (only PT_LOAD, PT_NULL, and PT_GNU_STACK are allowed).";
            return false;
        }

        if (phdr.p_flags == 0) {
            *out_error = "A PT_LOAD segment has no permissions set.";
            return false;
        }
        if ((phdr.p_flags & ~PF_KNOWN_MASK) != 0) {
            *out_error = "A PT_LOAD segment has unrecognized flag bits set.";
            return false;
        }

        if (phdr.p_memsz < phdr.p_filesz) {
            *out_error = "A segment's p_memsz is smaller than its p_filesz.";
            return false;
        }

        if (add_overflows(phdr.p_offset, phdr.p_filesz)) {
            *out_error = "A segment's file offset+size overflows.";
            return false;
        }
        if (phdr.p_offset + phdr.p_filesz > file_size) {
            *out_error = "A segment's data runs past end of file.";
            return false;
        }

        if (add_overflows(phdr.p_vaddr, phdr.p_memsz)) {
            *out_error = "A segment's virtual address+size overflows.";
            return false;
        }
        uint64_t seg_end = phdr.p_vaddr + phdr.p_memsz;

        if (phdr.p_vaddr < USER_SPACE_MIN) {
            *out_error = "A segment targets kernel-shared address space.";
            return false;
        }
        if (seg_end > USER_SPACE_MAX) {
            *out_error = "A segment targets non-canonical/reserved address space.";
            return false;
        }

        if (phdr.p_align != 0) {
            if (!is_power_of_two(phdr.p_align)) {
                *out_error = "A segment's alignment is not a power of two.";
                return false;
            }
            if ((phdr.p_vaddr % phdr.p_align) != (phdr.p_offset % phdr.p_align)) {
                *out_error = "A segment's virtual address and file offset disagree modulo its alignment.";
                return false;
            }
        }

        uint64_t page_start = phdr.p_vaddr & ~PAGE_MASK;
        uint64_t page_end = (seg_end + PAGE_MASK) & ~PAGE_MASK;
        if (page_end <= page_start) {
            *out_error = "A segment's page-aligned range is empty or overflowed.";
            return false;
        }

        for (int j = 0; j < load_count; ++j) {
            if (page_start < load_end[j] && load_start[j] < page_end) {
                *out_error = "Two PT_LOAD segments overlap.";
                return false;
            }
        }
        load_start[load_count] = page_start;
        load_end[load_count] = page_end;
        ++load_count;
    }

    if (load_count == 0) {
        *out_error = "ELF file has no PT_LOAD segments - nothing to run.";
        return false;
    }

    bool entry_ok = false;
    for (uint16_t i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr& phdr = phdrs[i];
        if (phdr.p_type != PT_LOAD) continue;
        if (header->e_entry >= phdr.p_vaddr && header->e_entry < phdr.p_vaddr + phdr.p_memsz) {
            entry_ok = true;
            break;
        }
    }
    if (!entry_ok) {
        *out_error = "Entry point does not fall within any loaded segment.";
        return false;
    }

    *out_header = header;
    *out_phdrs = phdrs;
    *out_phnum = header->e_phnum;
    return true;
}

} 
