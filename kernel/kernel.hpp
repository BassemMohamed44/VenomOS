// ============================================================
// kernel.hpp
// ============================================================
#pragma once

// Declared extern "C" so kernel_entry.asm can call it by its
// exact, unmangled symbol name.
extern "C" void kernel_main();
