#include "task.hpp"

#include "heap.hpp"
#include "interrupts.hpp"
#include "paging.hpp"
#include "ring3.hpp"
#include "scheduler.hpp"

extern "C" void switch_context(uint64_t* old_rsp_out, uint64_t new_rsp);

namespace task {

namespace {

constexpr int MAX_TASKS = 8;

Task tasks[MAX_TASKS] = {};
int current_index = -1;
Pid next_pid = 1;

uint64_t kernel_cr3 = 0;
uint64_t loaded_cr3 = 0;

struct PendingCleanup {
    void* stack_base;
    uint64_t cr3;
    bool active;
};
PendingCleanup pending_cleanup = {nullptr, 0, false};

void reap_pending_cleanup() {
    if (!pending_cleanup.active) return;

    asm volatile("cli");
    void* stack_base = pending_cleanup.stack_base;
    uint64_t cr3 = pending_cleanup.cr3;
    pending_cleanup.active = false;
    pending_cleanup.stack_base = nullptr;
    pending_cleanup.cr3 = 0;
    asm volatile("sti");

    if (stack_base != nullptr) heap::kfree(stack_base);
    if (cr3 != 0) paging::destroy_address_space(cr3);
}

inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

Task* find_free_slot() {
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (tasks[i].state == State::Dead) return &tasks[i];
    }
    return nullptr;
}

[[noreturn]] void idle_entry() {
    for (;;) {
        asm volatile("sti; hlt");
    }
}

Pid init_pid = NO_PID;

constexpr uint64_t INIT_IDLE_POLL_TICKS = 9;

[[noreturn]] void init_entry() {
    for (;;) {
        Pid pid;
        int exit_code;
        if (wait_for_child(&pid, &exit_code)) {
            continue;
        }
        sleep_current(INIT_IDLE_POLL_TICKS);
    }
}

}

extern "C" void task_trampoline();

namespace {

Task* init_common_slot(EntryFn entry, const char* name) {
    Task* slot = find_free_slot();
    if (slot == nullptr) return nullptr;

    void* stack = heap::kmalloc(TASK_STACK_SIZE);
    if (stack == nullptr) return nullptr;

    slot->state = State::New;

    uint8_t* stack_top = reinterpret_cast<uint8_t*>(stack) + TASK_STACK_SIZE;
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_top);

    *(--sp) = reinterpret_cast<uint64_t>(&task_trampoline);
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0x202;

    slot->rsp = reinterpret_cast<uint64_t>(sp);
    slot->stack_base = stack;
    slot->entry = entry;
    slot->pid = next_pid++;
    Task* parent = current();
    slot->parent_pid = (parent != nullptr) ? parent->pid : NO_PID;
    slot->exit_code = 0;
    slot->wake_tick = 0;
    slot->name = name;

    return slot;
}

}

extern "C" void task_trampoline() {

    reap_pending_cleanup();

    Task* self = current();
    if (self != nullptr && self->entry != nullptr) {
        self->entry();
    }

    exit_current(0);
}

void init() {
    for (int i = 0; i < MAX_TASKS; ++i) {
        tasks[i].state = State::Dead;
    }

    kernel_cr3 = read_cr3();
    loaded_cr3 = kernel_cr3;

    tasks[0].rsp = 0;
    tasks[0].stack_base = nullptr;
    tasks[0].entry = nullptr;
    tasks[0].state = State::Running;
    tasks[0].pid = next_pid++;
    tasks[0].parent_pid = NO_PID;
    tasks[0].exit_code = 0;
    tasks[0].wake_tick = 0;
    tasks[0].name = "shell";
    tasks[0].cr3 = 0;
    current_index = 0;

    Task* idle = create(&idle_entry, "idle");
    if (idle != nullptr) {
        idle->parent_pid = NO_PID;
    }

    Task* init_task = create(&init_entry, "init");
    if (init_task != nullptr) {
        init_task->parent_pid = NO_PID;
        init_pid = init_task->pid;
    }
}

Task* create(EntryFn entry, const char* name) {
    Task* slot = init_common_slot(entry, name);
    if (slot == nullptr) return nullptr;

    slot->cr3 = 0;
    slot->state = State::Ready;
    return slot;
}

Task* create_process(EntryFn entry, const char* name, uint64_t cr3,
                      uint64_t user_entry, uint64_t user_stack_top) {
    Task* slot = init_common_slot(entry, name);
    if (slot == nullptr) return nullptr;

    slot->cr3 = cr3;
    slot->user_entry = user_entry;
    slot->user_stack_top = user_stack_top;
    slot->state = State::Ready;
    return slot;
}

Task* current() {
    if (current_index < 0) return nullptr;
    return &tasks[current_index];
}

int capacity() { return MAX_TASKS; }

Task* at(int index) {
    if (index < 0 || index >= MAX_TASKS) return nullptr;
    return &tasks[index];
}

Task* find_by_pid(Pid pid) {
    if (pid == NO_PID) return nullptr;
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (tasks[i].state != State::Dead && tasks[i].pid == pid) return &tasks[i];
    }
    return nullptr;
}

void switch_to(Task* next) {
    if (next == nullptr) return;

    Task* prev = current();
    if (prev == next) return;

    if (prev != nullptr && prev->state == State::Running) {
        prev->state = State::Ready;
    }
    next->state = State::Running;

    for (int i = 0; i < MAX_TASKS; ++i) {
        if (&tasks[i] == next) {
            current_index = i;
            break;
        }
    }

    uint64_t target_cr3 = (next->cr3 != 0) ? next->cr3 : kernel_cr3;
    if (target_cr3 != loaded_cr3) {
        write_cr3(target_cr3);
        loaded_cr3 = target_cr3;
    }

    if (next->stack_base != nullptr) {
        uint64_t rsp0_top = reinterpret_cast<uint64_t>(next->stack_base) + TASK_STACK_SIZE;
        ring3::set_kernel_stack(rsp0_top);
    }

    if (prev == nullptr) {

        uint64_t discard;
        switch_context(&discard, next->rsp);
    } else {
        switch_context(&prev->rsp, next->rsp);
    }

    reap_pending_cleanup();
}

void block_current() {
    Task* self = current();
    if (self == nullptr) return;
    self->state = State::Blocked;
    scheduler::reschedule();

}

void unblock(Pid pid) {
    Task* t = find_by_pid(pid);
    if (t != nullptr && t->state == State::Blocked) {
        t->state = State::Ready;
    }
}

void sleep_current(uint64_t ticks_to_sleep) {
    Task* self = current();
    if (self == nullptr) return;
    self->wake_tick = interrupts::ticks() + ticks_to_sleep;
    self->state = State::Sleeping;
    scheduler::reschedule();

}

bool wait_for_child(Pid* out_pid, int* out_exit_code) {
    Task* self = current();
    if (self == nullptr) return false;

    bool has_any_child = false;
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (tasks[i].state != State::Dead && tasks[i].parent_pid == self->pid) {
            has_any_child = true;
            break;
        }
    }
    if (!has_any_child) return false;

    for (;;) {
        for (int i = 0; i < MAX_TASKS; ++i) {
            if (tasks[i].state == State::Zombie && tasks[i].parent_pid == self->pid) {
                if (out_pid != nullptr) *out_pid = tasks[i].pid;
                if (out_exit_code != nullptr) *out_exit_code = tasks[i].exit_code;
                tasks[i].state = State::Dead;
                tasks[i].parent_pid = NO_PID;
                return true;
            }
        }

        block_current();
    }
}

[[noreturn]] void exit_current(int exit_code) {
    Task* self = current();
    if (self != nullptr) {

        pending_cleanup.stack_base = self->stack_base;
        pending_cleanup.cr3 = self->cr3;
        pending_cleanup.active = (self->stack_base != nullptr) || (self->cr3 != 0);
        self->stack_base = nullptr;
        self->cr3 = 0;

        self->exit_code = exit_code;

        if (init_pid != NO_PID && self->pid != init_pid) {
            for (int i = 0; i < MAX_TASKS; ++i) {
                Task* child = &tasks[i];
                if (child->state != State::Dead && child->parent_pid == self->pid) {
                    child->parent_pid = init_pid;
                    unblock(init_pid);
                }
            }
        }

        bool has_live_parent = find_by_pid(self->parent_pid) != nullptr;
        if (has_live_parent) {
            self->state = State::Zombie;
            unblock(self->parent_pid);
        } else {
            self->state = State::Dead;
            self->parent_pid = NO_PID;
        }
    }

    scheduler::reschedule();

    for (;;) {
        asm volatile("cli; hlt");
    }
}

}
