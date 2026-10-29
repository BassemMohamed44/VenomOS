#include "scheduler.hpp"
#include "interrupts.hpp"
#include "task.hpp"

namespace scheduler {

namespace {

constexpr int TICKS_PER_SWITCH = 5;
int tick_counter = 0;

// Interrupts stay off while a task is chosen and switched to. task::switch_to()
// updates current() before switch_context() swaps stacks; a timer tick in that
// gap would run a second switch whose "previous task" is the one that has not
// started yet, overwriting the stack pointer saved for it. The previous
// interrupt state is restored from this task's own stack when it is resumed.
struct IrqGuard {
    uint64_t flags;
    IrqGuard() : flags(task::irq_save()) {}
    ~IrqGuard() { task::irq_restore(flags); }
    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

int index_of(task::Task* candidate) {
    for (int i = 0; i < task::capacity(); ++i) {
        if (task::at(i) == candidate) return i;
    }
    return -1;
}

void wake_sleeping_tasks() {
    uint64_t now = interrupts::ticks();
    for (int i = 0; i < task::capacity(); ++i) {
        task::Task* t = task::at(i);
        if (t != nullptr && t->state == task::State::Sleeping && now >= t->wake_tick) {
            t->state = task::State::Ready;
        }
    }
}

}

void tick() {
    wake_sleeping_tasks();

    if (++tick_counter < TICKS_PER_SWITCH) return;
    tick_counter = 0;
    reschedule();
}

void reschedule() {
    IrqGuard guard;
    task::Task* current = task::current();
    int start = index_of(current);
    if (start < 0) start = 0;

    for (int offset = 1; offset <= task::capacity(); ++offset) {
        int i = (start + offset) % task::capacity();
        task::Task* candidate = task::at(i);
        if (candidate == nullptr) continue;
        if (candidate->state == task::State::Ready) {
            task::switch_to(candidate);
            return;
        }
    }

}

void yield() {
    reschedule();
}

}
