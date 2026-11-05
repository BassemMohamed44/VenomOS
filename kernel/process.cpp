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

constexpr size_t MAX_ELF_SIZE = 64 * 1024;

void print_error(const char* message) {
    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    vga::print(message);
    vga::set_color(vga::Color::White, vga::Color::Black);
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

        uint64_t flags = segment_page_flags(phdr.p_flags);

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

}

uint64_t segment_page_flags(uint32_t p_flags) {
    uint64_t flags = paging::PAGE_PRESENT | paging::PAGE_USER | paging::PAGE_OWNED;
    if (p_flags & elf::PF_WRITE) {
        // Writable memory is never executable, whatever the ELF asked for.
        flags |= paging::PAGE_WRITABLE | paging::PAGE_NX;
    } else if (!(p_flags & elf::PF_EXEC)) {
        flags |= paging::PAGE_NX;
    }
    return flags;
}

bool build_initial_stack(uint8_t* window, size_t window_size,
                         int argc, const char* const* argv,
                         int envc, const char* const* envp,
                         uint64_t* out_sp) {
    if (argc < 0 || envc < 0 || argc > MAX_ARGS || envc > MAX_ENVS) return false;
    if (window_size != STACK_SIZE) return false;
    if ((argc > 0 && argv == nullptr) || (envc > 0 && envp == nullptr)) return false;

    const uint64_t top = STACK_VIRT + STACK_SIZE;

    size_t string_bytes = 0;
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr) return false;
        size_t n = 0;
        while (argv[i][n] != '\0') ++n;
        string_bytes += n + 1;
        if (string_bytes > MAX_ARG_BYTES) return false;
    }
    for (int i = 0; i < envc; ++i) {
        if (envp[i] == nullptr) return false;
        size_t n = 0;
        while (envp[i][n] != '\0') ++n;
        string_bytes += n + 1;
        if (string_bytes > MAX_ARG_BYTES) return false;
    }

    // argc + argv[] + NULL + envp[] + NULL + AT_NULL pair
    const size_t words = 1 + static_cast<size_t>(argc) + 1 + static_cast<size_t>(envc) + 1 + 2;
    const size_t pointer_bytes = words * 8;
    if (string_bytes + pointer_bytes + 16 > MAX_ARG_BYTES) return false;

    const uint64_t strings_base = top - string_bytes;
    const uint64_t sp = (strings_base - pointer_bytes) & ~static_cast<uint64_t>(15);

    // Zero the whole region first so padding is deterministic.
    for (uint64_t a = sp; a < top; ++a) window[a - STACK_VIRT] = 0;

    uint64_t str_cursor = strings_base;
    uint64_t word_cursor = sp;

    auto put_word = [&](uint64_t value) {
        uint8_t* dst = window + (word_cursor - STACK_VIRT);
        for (int b = 0; b < 8; ++b) dst[b] = static_cast<uint8_t>(value >> (8 * b));
        word_cursor += 8;
    };
    auto put_string = [&](const char* s) -> uint64_t {
        const uint64_t at = str_cursor;
        size_t n = 0;
        while (s[n] != '\0') {
            window[str_cursor - STACK_VIRT] = static_cast<uint8_t>(s[n]);
            ++str_cursor;
            ++n;
        }
        window[str_cursor - STACK_VIRT] = 0;
        ++str_cursor;
        return at;
    };

    put_word(static_cast<uint64_t>(argc));
    for (int i = 0; i < argc; ++i) put_word(put_string(argv[i]));
    put_word(0);
    for (int i = 0; i < envc; ++i) put_word(put_string(envp[i]));
    put_word(0);
    put_word(0);   // auxv: AT_NULL
    put_word(0);

    *out_sp = sp;
    return true;
}

bool run(const char* filename, int argc, const char* const* argv,
         int envc, const char* const* envp) {
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
        print_error("File is too large to run (max 64KB).\n");
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

    const char* default_argv[1] = { filename };
    if (argc == 0) { argc = 1; argv = default_argv; }

    static uint8_t stack_window[STACK_SIZE];
    uint64_t initial_sp = 0;
    if (!build_initial_stack(stack_window, STACK_SIZE, argc, argv, envc, envp, &initial_sp)) {
        paging::destroy_address_space(pml4);
        print_error("Too many or too long arguments/environment strings for the process stack.\n");
        return false;
    }

    bool stack_mapping_ok = true;
    for (uintptr_t offset = 0; offset < STACK_SIZE && stack_mapping_ok; offset += 4096) {
        uintptr_t stack_frame = pmm::alloc_frame();
        if (stack_frame == 0) {
            stack_mapping_ok = false;
            break;
        }
        {
            uint8_t* dst = reinterpret_cast<uint8_t*>(stack_frame);
            for (size_t b = 0; b < 4096; ++b) dst[b] = stack_window[offset + b];
        }
        if (!paging::map_page_in(pml4, STACK_VIRT + offset, stack_frame,
                paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER | paging::PAGE_OWNED | paging::PAGE_NX)) {
            pmm::free_frame(stack_frame);
            stack_mapping_ok = false;
        }
    }
    if (!stack_mapping_ok) {
        paging::destroy_address_space(pml4);
        print_error("Failed to map the process's stack.\n");
        return false;
    }

    if (paging::count_wx_pages(pml4) != 0) {
        paging::destroy_address_space(pml4);
        print_error("Refusing to run: the process image would have writable+executable pages.\n");
        return false;
    }

    if (task::create_process(&process_entry, filename, pml4,
                              header->e_entry, initial_sp) == nullptr) {
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

    // Segment permissions -> page flags (PF_R/PF_W/PF_X, W^X).
    {
        using namespace paging;
        const uint64_t r  = segment_page_flags(elf::PF_READ);
        const uint64_t rw = segment_page_flags(elf::PF_READ | elf::PF_WRITE);
        const uint64_t rx = segment_page_flags(elf::PF_READ | elf::PF_EXEC);
        const uint64_t wx = segment_page_flags(elf::PF_READ | elf::PF_WRITE | elf::PF_EXEC);
        const bool ok =
            (r  & PAGE_NX) && !(r  & PAGE_WRITABLE) &&
            (rw & PAGE_NX) &&  (rw & PAGE_WRITABLE) &&
            !(rx & PAGE_NX) && !(rx & PAGE_WRITABLE) &&
            ((wx & PAGE_WRITABLE) ? (wx & PAGE_NX) != 0 : true) &&
            (r & PAGE_USER) && (rx & PAGE_USER);
        ++total;
        if (ok) ++passed;
    }

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

    // Initial stack ABI: argc / argv / envp / auxv layout.
    {
        static uint8_t window[STACK_SIZE];
        const char* av[] = { "prog", "hello" };
        const char* ev[] = { "K=V" };
        uint64_t sp = 0;
        bool ok = build_initial_stack(window, STACK_SIZE, 2, av, 1, ev, &sp);
        auto rd = [&](uint64_t a) {
            uint64_t v = 0;
            for (int b = 7; b >= 0; --b) v = (v << 8) | window[a - STACK_VIRT + static_cast<uint64_t>(b)];
            return v;
        };
        auto str_eq = [&](uint64_t a, const char* s) {
            size_t i = 0;
            for (; s[i] != '\0'; ++i) if (window[a - STACK_VIRT + i] != static_cast<uint8_t>(s[i])) return false;
            return window[a - STACK_VIRT + i] == 0;
        };
        ok = ok && (sp % 16 == 0) && sp >= STACK_VIRT && sp < STACK_VIRT + STACK_SIZE;
        ok = ok && rd(sp) == 2;
        ok = ok && str_eq(rd(sp + 8), "prog") && str_eq(rd(sp + 16), "hello");
        ok = ok && rd(sp + 24) == 0;               // argv terminator
        ok = ok && str_eq(rd(sp + 32), "K=V");
        ok = ok && rd(sp + 40) == 0;               // envp terminator
        ok = ok && rd(sp + 48) == 0 && rd(sp + 56) == 0;   // AT_NULL
        ok = ok && rd(sp + 8) >= sp + 64 && rd(sp + 16) < STACK_VIRT + STACK_SIZE;

        // argc == 0 with no pointers, and limits
        uint64_t sp0 = 0;
        ok = ok && build_initial_stack(window, STACK_SIZE, 0, nullptr, 0, nullptr, &sp0);
        ok = ok && rd(sp0) == 0 && rd(sp0 + 8) == 0 && sp0 % 16 == 0;
        const char* many[MAX_ARGS + 1] = {};
        for (int i = 0; i <= MAX_ARGS; ++i) many[i] = "x";
        ok = ok && !build_initial_stack(window, STACK_SIZE, MAX_ARGS + 1, many, 0, nullptr, &sp0);
        static char big[MAX_ARG_BYTES + 8];
        for (size_t i = 0; i + 1 < sizeof(big); ++i) big[i] = 'a';
        big[sizeof(big) - 1] = '\0';
        const char* bigv[] = { big };
        ok = ok && !build_initial_stack(window, STACK_SIZE, 1, bigv, 0, nullptr, &sp0);
        ++total;
        if (ok) ++passed;
    }

    // A PT_LOAD segment that is both writable and executable must be rejected.
    if (first_load != -1) {
        auto* wx_phdr = reinterpret_cast<elf::Elf64_Phdr*>(buf + phoff + static_cast<size_t>(first_load) * sizeof(elf::Elf64_Phdr));
        for (size_t i = 0; i < total_size; ++i) buf[i] = demo_hello_start[i];
        wx_phdr->p_flags = elf::PF_READ | elf::PF_WRITE | elf::PF_EXEC;
        ++total;
        if (!elf::validate(buf, total_size, &h, &p, &n, &err)) ++passed;
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
