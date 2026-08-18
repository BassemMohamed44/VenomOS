#pragma once

#include "../include/stdint.hpp"

namespace interrupts {


struct [[gnu::packed]] InterruptFrame {
    
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;


    uint64_t vector;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};


void init();
uint64_t ticks();
extern "C" void interrupt_dispatch(InterruptFrame* frame);

} 
