#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace process {

constexpr uintptr_t STACK_GUARD_VIRT = 0x7FFFE000;
constexpr uintptr_t STACK_VIRT = 0x7FFFF000;
constexpr uintptr_t STACK_SIZE = 4 * 4096;

// Page-table flags for a PT_LOAD segment: writable only if PF_WRITE,
// executable (no PAGE_NX) only if PF_EXEC. Never both (W^X).
uint64_t segment_page_flags(uint32_t p_flags);

// ---- Initial process stack ABI (System V x86-64 process entry) -----------
// At the first instruction of _start, RSP is 16-byte aligned and points at:
//
//     [rsp]                 argc                (uint64_t)
//     [rsp + 8 ..]          argv[0] .. argv[argc-1], then a NULL pointer
//                           envp[0] .. envp[n-1],    then a NULL pointer
//                           auxv: a single AT_NULL pair (0, 0)
//     (above that, up to the stack top)   the NUL-terminated strings
//
// argv[0] is the program's file name. All pointers are user virtual
// addresses inside the process's own stack. No register carries arguments.
constexpr int MAX_ARGS = 16;
constexpr int MAX_ENVS = 8;
constexpr size_t MAX_ARG_BYTES = 2048;   // strings + pointer block must fit in this

// Lays the argc/argv/envp image out into `window`, which stands for the user
// stack range [STACK_VIRT, STACK_VIRT + STACK_SIZE) (window_size == STACK_SIZE).
// On success stores the initial user RSP in *out_sp. Fails (false) if there are
// too many arguments or they do not fit in MAX_ARG_BYTES. `argv`/`envp` may be
// null when the matching count is 0.
bool build_initial_stack(uint8_t* window, size_t window_size,
                         int argc, const char* const* argv,
                         int envc, const char* const* envp,
                         uint64_t* out_sp);

// Runs an ELF file. argv[0] defaults to `filename` when argc is 0.
bool run(const char* filename, int argc = 0, const char* const* argv = nullptr,
         int envc = 0, const char* const* envp = nullptr, uint64_t* out_pid = nullptr);

// Starts `program` with the space-separated `args` (may be null) and the
// default environment. argv[0] is the program name. Used by the shell-less
// callers (the spawn syscall). Stores the child's PID in *out_pid.
bool spawn_line(const char* program, const char* args, uint64_t* out_pid);

bool self_test();

}
