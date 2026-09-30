#include "task.hpp"

#include "heap.hpp"
#include "interrupts.hpp"
#include "paging.hpp"
#include "ring3.hpp"
#include "scheduler.hpp"

extern "C" void switch_context(uint64_t* old_rsp_out, uint64_t new_rsp);

namespace task {

namespace {

constexpr int MAX_TASKS = 128;
constexpr int PID_INDEX_BITS = 8;
constexpr uint64_t PID_INDEX_FIELD_MASK = (1ull << PID_INDEX_BITS) - 1;
constexpr uint64_t PID_GENERATION_MAX = ~0ull;

static_assert(MAX_TASKS <= (1 << PID_INDEX_BITS) - 1, "MAX_TASKS no longer fits in PID_INDEX_BITS");

Task tasks[MAX_TASKS] = {};
int current_index = -1;

Pid make_pid(int slot_index, uint64_t generation) {
    return (generation << PID_INDEX_BITS) | (static_cast<uint64_t>(slot_index) + 1);
}

int pid_slot_index(Pid pid) {
    return static_cast<int>((pid & PID_INDEX_FIELD_MASK) - 1);
}

uint64_t pid_generation(Pid pid) {
    return pid >> PID_INDEX_BITS;
}

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
        if (tasks[i].state == State::Dead && tasks[i].generation != PID_GENERATION_MAX) {
            return &tasks[i];
        }
    }
    return nullptr;
}

void add_child(Task* parent, Task* child) {
    child->next_sibling = parent->first_child;
    parent->first_child = child->pid;
}

void remove_child(Task* parent, Pid child_pid) {
    if (parent->first_child == child_pid) {
        Task* child = find_by_pid(child_pid);
        parent->first_child = (child != nullptr) ? child->next_sibling : NO_PID;
        return;
    }
    Pid prev_pid = parent->first_child;
    while (prev_pid != NO_PID) {
        Task* prev = find_by_pid(prev_pid);
        if (prev == nullptr) return;
        if (prev->next_sibling == child_pid) {
            Task* child = find_by_pid(child_pid);
            prev->next_sibling = (child != nullptr) ? child->next_sibling : NO_PID;
            return;
        }
        prev_pid = prev->next_sibling;
    }
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
    slot->generation += 1;
    slot->pid = make_pid(static_cast<int>(slot - tasks), slot->generation);
    Task* parent = current();
    slot->parent_pid = (parent != nullptr) ? parent->pid : NO_PID;
    slot->first_child = NO_PID;
    slot->next_sibling = NO_PID;
    if (parent != nullptr) {
        add_child(parent, slot);
    }
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
    tasks[0].generation += 1;
    tasks[0].pid = make_pid(0, tasks[0].generation);
    tasks[0].parent_pid = NO_PID;
    tasks[0].first_child = NO_PID;
    tasks[0].next_sibling = NO_PID;
    tasks[0].exit_code = 0;
    tasks[0].wake_tick = 0;
    tasks[0].name = "shell";
    tasks[0].cr3 = 0;
    current_index = 0;

    Task* idle = create(&idle_entry, "idle");
    if (idle != nullptr) {
        remove_child(&tasks[0], idle->pid);
        idle->parent_pid = NO_PID;
    }

    Task* init_task = create(&init_entry, "init");
    if (init_task != nullptr) {
        remove_child(&tasks[0], init_task->pid);
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
    int index = pid_slot_index(pid);
    if (index < 0 || index >= MAX_TASKS) return nullptr;
    Task* t = &tasks[index];
    if (t->state == State::Dead) return nullptr;
    if (t->pid != pid) return nullptr;
    return t;
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

    if (self->first_child == NO_PID) return false;

    for (;;) {
        Pid child_pid = self->first_child;
        while (child_pid != NO_PID) {
            Task* child = find_by_pid(child_pid);
            if (child == nullptr) break;
            if (child->state == State::Zombie) {
                if (out_pid != nullptr) *out_pid = child->pid;
                if (out_exit_code != nullptr) *out_exit_code = child->exit_code;
                remove_child(self, child->pid);
                child->state = State::Dead;
                child->parent_pid = NO_PID;
                return true;
            }
            child_pid = child->next_sibling;
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
            Task* init_task = find_by_pid(init_pid);
            Pid child_pid = self->first_child;
            while (child_pid != NO_PID) {
                Task* child = find_by_pid(child_pid);
                if (child == nullptr) break;
                Pid next_sibling_pid = child->next_sibling;
                child->parent_pid = init_pid;
                child->next_sibling = NO_PID;
                if (init_task != nullptr) {
                    add_child(init_task, child);
                }
                unblock(init_pid);
                child_pid = next_sibling_pid;
            }
            self->first_child = NO_PID;
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

namespace {

constexpr int LIFECYCLE_TEST_EXIT_CODE = 42;
bool lifecycle_ran_sleep_phase = false;
bool lifecycle_ran_block_phase = false;

void lifecycle_test_child_entry() {
    sleep_current(3);
    lifecycle_ran_sleep_phase = true;
    block_current();
    lifecycle_ran_block_phase = true;
    exit_current(LIFECYCLE_TEST_EXIT_CODE);
}

constexpr int ORPHAN_TEST_MID_EXIT_CODE = 3;
constexpr int ORPHAN_TEST_GRANDCHILD_EXIT_CODE = 7;
Pid orphan_test_grandchild_pid = NO_PID;

void orphan_test_grandchild_entry() {
    sleep_current(5);
    exit_current(ORPHAN_TEST_GRANDCHILD_EXIT_CODE);
}

void orphan_test_mid_entry() {
    Task* g = create(&orphan_test_grandchild_entry, "orphan-test-grandchild");
    orphan_test_grandchild_pid = (g != nullptr) ? g->pid : NO_PID;
    exit_current(ORPHAN_TEST_MID_EXIT_CODE);
}

bool child_list_contains(Task* parent, Pid target_pid) {
    Pid p = parent->first_child;
    while (p != NO_PID) {
        Task* t = find_by_pid(p);
        if (t == nullptr) return false;
        if (t->pid == target_pid) return true;
        p = t->next_sibling;
    }
    return false;
}

constexpr int TABLE_EXHAUSTION_TEST_EXIT_CODE = 55;

void table_exhaustion_test_entry() {
    exit_current(TABLE_EXHAUSTION_TEST_EXIT_CODE);
}

}

bool self_test() {
    bool ok = true;
    Task* self = current();
    if (self == nullptr) return false;

    lifecycle_ran_sleep_phase = false;
    lifecycle_ran_block_phase = false;

    Task* child = create(&lifecycle_test_child_entry, "lifecycle-test-child");
    if (child == nullptr) return false;
    Pid child_pid = child->pid;

    if (child->state != State::Ready) ok = false;
    if (child->parent_pid != self->pid) ok = false;
    if (!child_list_contains(self, child_pid)) ok = false;

    int attempts = 0;
    while (!lifecycle_ran_sleep_phase && attempts < 20) {
        sleep_current(2);
        ++attempts;
    }
    if (!lifecycle_ran_sleep_phase) ok = false;

    attempts = 0;
    while (child->state != State::Blocked && attempts < 20) {
        sleep_current(1);
        ++attempts;
    }
    if (child->state != State::Blocked) ok = false;

    unblock(child_pid);

    attempts = 0;
    while (child->state != State::Zombie && attempts < 20) {
        sleep_current(1);
        ++attempts;
    }
    if (child->state != State::Zombie) ok = false;
    if (!lifecycle_ran_block_phase) ok = false;
    if (child->exit_code != LIFECYCLE_TEST_EXIT_CODE) ok = false;

    Pid reaped_pid = NO_PID;
    int reaped_code = 0;
    bool reaped = wait_for_child(&reaped_pid, &reaped_code);
    if (!reaped) ok = false;
    if (reaped_pid != child_pid) ok = false;
    if (reaped_code != LIFECYCLE_TEST_EXIT_CODE) ok = false;
    if (find_by_pid(child_pid) != nullptr) ok = false;

    orphan_test_grandchild_pid = NO_PID;
    Task* mid = create(&orphan_test_mid_entry, "orphan-test-mid");
    if (mid == nullptr) return false;
    Pid mid_pid = mid->pid;

    attempts = 0;
    while (orphan_test_grandchild_pid == NO_PID && attempts < 20) {
        sleep_current(1);
        ++attempts;
    }
    Pid grandchild_pid = orphan_test_grandchild_pid;
    if (grandchild_pid == NO_PID) ok = false;

    Task* grandchild = nullptr;
    attempts = 0;
    while (attempts < 20) {
        grandchild = find_by_pid(grandchild_pid);
        if (grandchild != nullptr && grandchild->parent_pid == init_pid) break;
        sleep_current(1);
        ++attempts;
    }
    if (grandchild == nullptr || grandchild->parent_pid != init_pid) {
        ok = false;
    } else {
        Task* init_task = find_by_pid(init_pid);
        if (init_task == nullptr || !child_list_contains(init_task, grandchild_pid)) {
            ok = false;
        }
    }

    Pid mid_reaped_pid = NO_PID;
    int mid_reaped_code = 0;
    bool mid_reaped = false;
    attempts = 0;
    while (!mid_reaped && attempts < 20) {
        mid_reaped = wait_for_child(&mid_reaped_pid, &mid_reaped_code);
        if (!mid_reaped) sleep_current(1);
        ++attempts;
    }
    if (!mid_reaped) ok = false;
    if (mid_reaped_pid != mid_pid) ok = false;
    if (mid_reaped_code != ORPHAN_TEST_MID_EXIT_CODE) ok = false;

    attempts = 0;
    while (find_by_pid(grandchild_pid) != nullptr && attempts < 30) {
        sleep_current(2);
        ++attempts;
    }
    if (find_by_pid(grandchild_pid) != nullptr) ok = false;

    Task* filled[MAX_TASKS];
    Pid filled_pids[MAX_TASKS];
    int filled_count = 0;
    bool hit_exhaustion = false;
    for (int i = 0; i < MAX_TASKS + 1; ++i) {
        Task* t = create(&table_exhaustion_test_entry, "exhaustion-test");
        if (t == nullptr) {
            hit_exhaustion = true;
            break;
        }
        filled[filled_count] = t;
        filled_pids[filled_count] = t->pid;
        ++filled_count;
    }
    if (!hit_exhaustion) ok = false;

    for (int i = 0; i < filled_count; ++i) {
        for (int j = i + 1; j < filled_count; ++j) {
            if (filled_pids[i] == filled_pids[j]) ok = false;
        }
    }

    for (int i = 0; i < filled_count; ++i) {
        Pid pid = NO_PID;
        int code = 0;
        if (!wait_for_child(&pid, &code)) ok = false;
        if (code != TABLE_EXHAUSTION_TEST_EXIT_CODE) ok = false;
    }

    Task* recovery = create(&table_exhaustion_test_entry, "exhaustion-test-recovery");
    if (recovery == nullptr) {
        ok = false;
    } else {
        int recovery_slot = pid_slot_index(recovery->pid);
        if (pid_generation(recovery->pid) <= 1) ok = false;

        for (int i = 0; i < filled_count; ++i) {
            if (pid_slot_index(filled_pids[i]) == recovery_slot) {
                if (filled_pids[i] == recovery->pid) ok = false;
                if (find_by_pid(filled_pids[i]) != nullptr) ok = false;
                break;
            }
        }

        Pid pid = NO_PID;
        int code = 0;
        attempts = 0;
        bool recovery_reaped = false;
        while (!recovery_reaped && attempts < 20) {
            recovery_reaped = wait_for_child(&pid, &code);
            if (!recovery_reaped) sleep_current(1);
            ++attempts;
        }
        if (!recovery_reaped) ok = false;
        if (code != TABLE_EXHAUSTION_TEST_EXIT_CODE) ok = false;
    }

    {
        int poison_index = -1;
        for (int i = 0; i < MAX_TASKS; ++i) {
            if (tasks[i].state == State::Dead) { poison_index = i; break; }
        }
        if (poison_index < 0) {
            ok = false;
        } else {
            uint64_t backup_generation = tasks[poison_index].generation;

            Pid encoded = make_pid(poison_index, 12345);
            if (pid_slot_index(encoded) != poison_index) ok = false;
            if (pid_generation(encoded) != 12345) ok = false;

            tasks[poison_index].generation = PID_GENERATION_MAX;
            Task* found = find_free_slot();
            bool skipped_exhausted_slot = (found == nullptr) || (found != &tasks[poison_index]);
            if (!skipped_exhausted_slot) ok = false;

            tasks[poison_index].generation = backup_generation;
        }
    }

    return ok;
}

}
