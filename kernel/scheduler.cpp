#include "scheduler.hpp"
#include "interrupts.hpp"
#include "task.hpp"

namespace scheduler {

namespace {

constexpr int TICKS_PER_SWITCH = 5;
int tick_counter = 0;

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
