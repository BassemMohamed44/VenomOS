#pragma once

#include "../include/stdint.hpp"
#include "../include/stddef.hpp"

namespace task {

enum class State {
    Dead,
    New,
    Ready,
    Running,
    Blocked,
    Sleeping,
    Zombie,
};

using EntryFn = void (*)();
using Pid = uint64_t;

constexpr size_t TASK_STACK_SIZE = 16384; 
constexpr Pid NO_PID = 0;                  

struct Task {
    uint64_t rsp;       
    void* stack_base;    
    EntryFn entry;        
    State state;
    Pid pid;
    Pid parent_pid;       
    int exit_code;         
    uint64_t wake_tick;     
    const char* name;
    uint64_t cr3;         
    uint64_t user_entry;      
    uint64_t user_stack_top;  
};

void init();

Task* create(EntryFn entry, const char* name);

Task* create_process(EntryFn entry, const char* name, uint64_t cr3,
                      uint64_t user_entry, uint64_t user_stack_top);

Task* current();
int capacity();
Task* at(int index);


Task* find_by_pid(Pid pid);


void switch_to(Task* next);


void block_current();

void unblock(Pid pid);

void sleep_current(uint64_t ticks_to_sleep);

bool wait_for_child(Pid* out_pid, int* out_exit_code);

[[noreturn]] void exit_current(int exit_code);

}
