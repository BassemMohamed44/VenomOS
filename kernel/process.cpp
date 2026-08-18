#include "process.hpp"

#include "elf.hpp"
#include "fs.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "ring3.hpp"
#include "task.hpp"
#include "vga.hpp"

extern "C" uint8_t demo_hello_start[];
extern "C" uint8_t demo_hello_end[];

namespace process {

namespace {

constexpr uintptr_t STACK_VIRT = 0x7FFFF000; 
constexpr uintptr_t STACK_SIZE = 4096;

constexpr size_t MAX_ELF_SIZE = 64 * 1024;

void print_error(const char* message) {
    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    vga::print(message);
    vga::set_color(vga::Color::White, vga::Color::Black);
}
uint64_t translate_segment_flags(uint32_t p_flags) {
    uint64_t flags = paging::PAGE_PRESENT | paging::PAGE_USER;
    if (p_flags & elf::PF_WRITE) flags |= paging::PAGE_WRITABLE;
    return flags;
}

void process_entry() {
    ring3::setup_tss();
    task::Task* self = task::current();
    ring3::enter(self->user_entry, self->user_stack_top);
}

bool load_segments(const uint8_t* file_buffer, const elf::Elf64_Ehdr* header,
                    const elf::Elf64_Phdr* phdrs, uint16_t phnum, uint64_t pml4) {
    for (uint16_t i = 0; i < phnum; ++i) {
        const elf::Elf64_Phdr& phdr = phdrs[i];
        if (phdr.p_type != elf::PT_LOAD) continue;

        uint64_t flags = translate_segment_flags(phdr.p_flags);

        uint64_t page_start = phdr.p_vaddr & ~static_cast<uint64_t>(0xFFF);
        uint64_t page_end = (phdr.p_vaddr + phdr.p_memsz + 0xFFF) & ~static_cast<uint64_t>(0xFFF);
        uint64_t page_count = (page_end - page_start) / 4096;

        for (uint64_t p = 0; p < page_count; ++p) {
            uintptr_t frame = pmm::alloc_frame();
            if (frame == 0) {
                print_error("Out of physical memory while loading the process.\n");
                return false;
            }

            uint8_t* dest = reinterpret_cast<uint8_t*>(frame);
            for (size_t b = 0; b < 4096; ++b) dest[b] = 0;  

            uint64_t frame_vaddr = page_start + p * 4096;

            uint64_t seg_data_start = phdr.p_vaddr;
            uint64_t seg_data_end = phdr.p_vaddr + phdr.p_filesz;
            uint64_t copy_start = frame_vaddr > seg_data_start ? frame_vaddr : seg_data_start;
            uint64_t copy_end = (frame_vaddr + 4096) < seg_data_end ? (frame_vaddr + 4096) : seg_data_end;

            if (copy_end > copy_start) {
                uint64_t file_off = phdr.p_offset + (copy_start - phdr.p_vaddr);
                uint64_t page_off = copy_start - frame_vaddr;
                uint64_t len = copy_end - copy_start;
                for (uint64_t b = 0; b < len; ++b) {
                    dest[page_off + b] = file_buffer[file_off + b];
                }
            }

            if (!paging::map_page_in(pml4, frame_vaddr, frame, flags)) {
    
                pmm::free_frame(frame);
                print_error("Failed to map a segment of the process.\n");
                return false;
            }
        }
    }

    (void)header;
    return true;
}

} // namespace

bool run(const char* filename) {
    if (!fs::exists(filename)) {
        print_error("File not found.\n");
        return false;
    }

    int64_t size = fs::file_size(filename);
    if (size <= 0) {
        print_error("Cannot run an empty file.\n");
        return false;
    }
    if (static_cast<size_t>(size) > MAX_ELF_SIZE) {
        print_error("File is too large to run (max 64KB in this phase).\n");
        return false;
    }

    static uint8_t file_buffer[MAX_ELF_SIZE];
    size_t bytes_read = 0;
    if (!fs::read(filename, file_buffer, MAX_ELF_SIZE, &bytes_read) || bytes_read == 0) {
        print_error("Failed to read the file.\n");
        return false;
    }

    const elf::Elf64_Ehdr* header = nullptr;
    const elf::Elf64_Phdr* phdrs = nullptr;
    uint16_t phnum = 0;
    const char* validation_error = nullptr;

    if (!elf::validate(file_buffer, bytes_read, &header, &phdrs, &phnum, &validation_error)) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Rejected: ");
        vga::print(validation_error != nullptr ? validation_error : "unknown validation failure.");
        vga::put_char('\n');
        vga::set_color(vga::Color::White, vga::Color::Black);
        return false;
    }

    uint64_t pml4 = paging::create_address_space();
    if (pml4 == 0) {
        print_error("Failed to create an address space (out of memory).\n");
        return false;
    }

    if (!load_segments(file_buffer, header, phdrs, phnum, pml4)) {
        paging::destroy_address_space(pml4);
        return false;
    }

    uintptr_t stack_frame = pmm::alloc_frame();
    if (stack_frame == 0) {
        paging::destroy_address_space(pml4);
        print_error("Failed to allocate the process's stack (out of memory).\n");
        return false;
    }
    if (!paging::map_page_in(pml4, STACK_VIRT, stack_frame,
            paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER)) {
        pmm::free_frame(stack_frame);
        paging::destroy_address_space(pml4);
        print_error("Failed to map the process's stack.\n");
        return false;
    }

    if (task::create_process(&process_entry, filename, pml4,
                              header->e_entry, STACK_VIRT + STACK_SIZE) == nullptr) {
        paging::destroy_address_space(pml4);
        print_error("Failed to create a task for this process (out of task slots or memory).\n");
        return false;
    }

    return true;
}

bool self_test() {
    size_t total_size = static_cast<size_t>(demo_hello_end - demo_hello_start);
    if (total_size == 0 || total_size > MAX_ELF_SIZE) return false;

    static uint8_t buf[MAX_ELF_SIZE];
    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];

    const elf::Elf64_Ehdr* h = nullptr;
    const elf::Elf64_Phdr* p = nullptr;
    uint16_t n = 0;
    const char* err = nullptr;

    int passed = 0;
    int total = 0;

    ++total;
    if (elf::validate(buf, total_size, &h, &p, &n, &err)) {
        ++passed;
    } else {
        return false;
    }

    size_t phoff = h->e_phoff;
    int first_load = -1;
    int second_load = -1;
    for (uint16_t i = 0; i < n; ++i) {
        if (p[i].p_type == elf::PT_LOAD) {
            if (first_load == -1) first_load = i;
            else if (second_load == -1) second_load = i;
        }
    }


    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
    buf[0] = 0x00;
    ++total;
    if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
    ++total;
    if (!elf::validate(buf, sizeof(elf::Elf64_Ehdr) - 1, &h, &p, &n, &err)) ++passed;

    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
    reinterpret_cast<elf::Elf64_Ehdr*>(buf)->e_type = 3;
    ++total;
    if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
    reinterpret_cast<elf::Elf64_Ehdr*>(buf)->e_machine = 3;
    ++total;
    if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

    if (first_load != -1) {
        auto* first_phdr = reinterpret_cast<elf::Elf64_Phdr*>(buf + phoff + static_cast<size_t>(first_load) * sizeof(elf::Elf64_Phdr));

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        if (first_phdr->p_filesz > 0) first_phdr->p_memsz = first_phdr->p_filesz - 1;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_vaddr = 0x1000;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_align = 3;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_flags = 0;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;
        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_flags = 0xF8;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_filesz = static_cast<uint64_t>(total_size) + 0x1000;
        first_phdr->p_memsz = first_phdr->p_filesz;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_offset = ~static_cast<uint64_t>(0); 
        first_phdr->p_filesz = 1;
        first_phdr->p_memsz = 1;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_align = 0x1000;
        first_phdr->p_vaddr += 7;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_vaddr = 0x0000900000000000ull;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;


        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        first_phdr->p_type = 2; 
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;
    }

    for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
    reinterpret_cast<elf::Elf64_Ehdr*>(buf)->e_entry = 0x50000000ull;
    ++total;
    if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;

    if (first_load != -1 && second_load != -1) {
        auto* first_phdr = reinterpret_cast<elf::Elf64_Phdr*>(buf + phoff + static_cast<size_t>(first_load) * sizeof(elf::Elf64_Phdr));
        auto* second_phdr = reinterpret_cast<elf::Elf64_Phdr*>(buf + phoff + static_cast<size_t>(second_load) * sizeof(elf::Elf64_Phdr));

        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        second_phdr->p_vaddr = first_phdr->p_vaddr;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;
    }

    return passed == total;
}

}
