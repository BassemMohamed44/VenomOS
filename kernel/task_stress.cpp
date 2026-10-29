#include "task.hpp"

#include "heap.hpp"
#include "interrupts.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "scheduler.hpp"
#include "vga.hpp"

// Scheduler stress suite.
//
// task::self_test() proves each lifecycle edge once. This file hammers them:
// repeated create / sleep / wake / block / exit cycles, timer preemption of
// CPU-bound tasks, PID-slot reuse, task-table exhaustion, orphan adoption,
// per-process address-space teardown and a seeded random mix of all of it.
// After every test it checks the structural invariants of the task table, and
// that no task slot, kernel-heap byte or physical frame was leaked.
//
// How the tests are shaped (all of it follows from the scheduler itself, which
// is round-robin by slot index, switches on every 5th timer tick, and has an
// `idle` task that is always Ready):
//  - Everything runs inside one dedicated "stress-runner" task, so the
//    children it creates are the only children it has, and wait_for_child()
//    can never reap something that belongs to somebody else.
//  - Handing the CPU over can cost up to one 5-tick quantum (idle halts until
//    the next tick), so work is done in batches, not one task at a time.
//  - A timer tick can land between any two instructions. Checks that need a
//    consistent snapshot ("nothing has run since X") are wrapped in
//    irq_save()/irq_restore(); every wait polls against a deadline, so a
//    scheduler bug shows up as a FAIL line instead of a hung shell.
//  - wait_for_child() blocks when children exist but none is a Zombie, so it
//    is only ever called after a Zombie child has been observed.
//  - Tasks find their own bookkeeping by slot index, not by creation order, so
//    a child that runs before its creator has stored anything cannot race.

namespace task {

namespace {

constexpr int MAX_SLOTS = 128;          // arrays below; capacity() is checked against it
constexpr uint64_t QUANTUM_TICKS = 5;   // scheduler::TICKS_PER_SWITCH

struct Params {
    int create_batches;   // test 1: batches of 16 create/exit/reap
    int sleep_rounds;     // test 2: sleep/wake rounds per sleeper
    int block_rounds;     // test 3: block/unblock rounds
    int spinners;         // test 4: CPU-bound tasks
    int spin_windows;     // test 4: observation windows
    int exhaust_rounds;   // test 5: fill the task table this many times
    int proc_rounds;      // test 6: batches of private-address-space processes
    int orphan_rounds;    // test 7: parents that die with live children
    int churn_ops;        // test 8: random operations
    int heap_ticks;       // test 9: minimum run time, in timer ticks
    int heap_rounds;      // test 9: minimum allocate/free rounds per task
};

constexpr Params QUICK = {2, 1, 2, 2, 1, 2, 2, 1, 30, 20, 300};
constexpr Params FULL  = {10, 4, 8, 4, 2, 6, 6, 3, 120, 60, 3000};

Params g_params = QUICK;
bool g_full = false;

volatile bool g_runner_done = false;
volatile bool g_runner_ok = false;

int g_test_failures = 0;
int g_failure_lines = 0;
const char* g_test_name = "";
constexpr int MAX_FAILURE_LINES = 12;

// ---- small helpers -------------------------------------------------------

void print_uint(uint64_t value) {
    char buffer[21];
    int length = 0;
    if (value == 0) buffer[length++] = '0';
    while (value > 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (length > 0) vga::put_char(buffer[--length]);
}

void print_status(bool ok) {
    if (ok) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        vga::print("PASS");
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("FAIL");
    }
    vga::set_color(vga::Color::White, vga::Color::Black);
}

bool str_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}

bool expect(bool condition, const char* what) {
    if (condition) return true;
    ++g_test_failures;
    if (g_failure_lines < MAX_FAILURE_LINES) {
        ++g_failure_lines;
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("\n  FAIL [");
        vga::print(g_test_name);
        vga::print("]: ");
        vga::print(what);
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
    return false;
}

struct IrqGuard {
    uint64_t flags;
    IrqGuard() : flags(irq_save()) {}
    ~IrqGuard() { irq_restore(flags); }
    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

int count_state(State state) {
    int count = 0;
    for (int i = 0; i < capacity(); ++i) {
        if (at(i)->state == state) ++count;
    }
    return count;
}

int count_dead_slots() { return count_state(State::Dead); }

int self_slot() {
    return pid_slot_of(current()->pid);
}

// Walks a parent's child list. The step bound turns a cyclic list (a bug)
// into "not found" instead of an endless loop.
bool child_list_has(const Task* parent, Pid target) {
    Pid p = parent->first_child;
    for (int steps = 0; p != NO_PID && steps <= capacity(); ++steps) {
        const Task* t = find_by_pid(p);
        if (t == nullptr) return false;
        if (p == target) return true;
        p = t->next_sibling;
    }
    return false;
}

Task* find_init() {
    for (int i = 0; i < capacity(); ++i) {
        Task* t = at(i);
        if (t->state != State::Dead && t->parent_pid == NO_PID && str_equal(t->name, "init")) return t;
    }
    return nullptr;
}

// Structural invariants of the task table, true at every instant the caller
// can observe it.
void check_invariants() {
    IrqGuard guard;

    Task* self = current();
    expect(count_state(State::Running) == 1, "there is not exactly one Running task");
    expect(self != nullptr && self->state == State::Running, "current() is not the Running task");

    for (int i = 0; i < capacity(); ++i) {
        Task* t = at(i);

        if (t->state == State::Dead || t->state == State::Zombie) {
            expect(t->stack_base == nullptr, "a Dead/Zombie task still owns its kernel stack");
            expect(t->cr3 == 0, "a Dead/Zombie task still owns an address space");
        }
        if (t->state == State::Zombie) {
            expect(find_by_pid(t->parent_pid) != nullptr, "a Zombie has no live parent to reap it");
        }
        if (t->state != State::Dead && t->parent_pid != NO_PID) {
            Task* parent = find_by_pid(t->parent_pid);
            if (expect(parent != nullptr, "a live task points at a parent that no longer exists")) {
                expect(child_list_has(parent, t->pid), "a task is missing from its parent's child list");
            }
        }
    }
}

// The last exited task's stack is freed by whichever task runs next, so give
// the scheduler a few chances to do that before calling it a leak.
bool settle_heap(size_t expected) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (heap::bytes_in_use() == expected) return true;
        scheduler::yield();
    }
    return heap::bytes_in_use() == expected;
}

bool settle_frames(uint64_t expected) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (pmm::used_frames() == expected) return true;
        scheduler::yield();
    }
    return pmm::used_frames() == expected;
}

// Polls (yielding the CPU) until every pid is a Zombie, or the deadline
// passes. Returns true only if all of them got there.
bool await_zombies(const Pid* pids, int count, uint64_t timeout_ticks) {
    const uint64_t deadline = interrupts::ticks() + timeout_ticks;
    for (;;) {
        int zombies = 0;
        for (int i = 0; i < count; ++i) {
            Task* t = find_by_pid(pids[i]);
            if (t != nullptr && t->state == State::Zombie) ++zombies;
        }
        if (zombies == count) return true;
        if (interrupts::ticks() > deadline) return false;
        scheduler::yield();
    }
}

bool any_zombie(const Pid* pids, int count) {
    for (int i = 0; i < count; ++i) {
        Task* t = find_by_pid(pids[i]);
        if (t != nullptr && t->state == State::Zombie) return true;
    }
    return false;
}

// Reaps `count` children that are already Zombies and checks each one against
// the pids we created. If `codes_are_slots`, the exit code must equal the
// child's slot index, which is what the standard test children exit with.
void reap_and_verify(const Pid* pids, int count, bool codes_are_slots) {
    bool seen[MAX_SLOTS] = {};
    for (int r = 0; r < count; ++r) {
        // wait_for_child() would block forever if no child is a Zombie.
        if (!expect(any_zombie(pids, count), "expected a Zombie child to reap but found none")) break;

        Pid pid = NO_PID;
        int code = 0;
        if (!expect(wait_for_child(&pid, &code), "wait_for_child found no child to reap")) break;

        int index = -1;
        for (int i = 0; i < count; ++i) {
            if (pids[i] == pid) index = i;
        }
        if (expect(index >= 0, "reaped a pid this test did not create")) {
            expect(!seen[index], "same pid reaped twice");
            seen[index] = true;
        }
        if (codes_are_slots) {
            expect(code == pid_slot_of(pid), "child exit code was lost or wrong");
        }
        expect(find_by_pid(pid) == nullptr, "reaped pid still resolves (stale pid accepted)");
        expect(at(pid_slot_of(pid))->state == State::Dead, "reaped slot is not Dead");
    }
}

// Standard child: exits at once with its own slot index.
void exit_with_slot_entry() {
    exit_current(self_slot());
}

// ---- test 1: repeated create / exit / reap batches -----------------------
// State path: (New) -> Ready -> Running -> Zombie -> Dead, many times over,
// through the same few slots.

void test_create_exit_reap() {
    constexpr int BATCH = 16;
    Task* self = current();

    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();
    uint64_t gens_before[MAX_SLOTS];
    bool slot_used[MAX_SLOTS] = {};
    int distinct_slots = 0;

    for (int batch = 0; batch < g_params.create_batches; ++batch) {
        for (int i = 0; i < capacity(); ++i) gens_before[i] = at(i)->generation;

        Pid pids[BATCH];
        int created = 0;
        for (int i = 0; i < BATCH; ++i) {
            IrqGuard guard;
            Task* t = create(&exit_with_slot_entry, "stress-exit");
            if (!expect(t != nullptr, "create() failed with free slots available")) break;

            pids[created++] = t->pid;
            const int slot = pid_slot_of(t->pid);
            expect(t->state == State::Ready, "new task is not Ready");
            expect(t->parent_pid == self->pid, "new task has the wrong parent");
            expect(at(slot) == t && find_by_pid(t->pid) == t, "pid does not resolve to its task");
            expect(pid_generation_of(t->pid) == gens_before[slot] + 1,
                   "slot generation did not advance by exactly one");
            if (!slot_used[slot]) { slot_used[slot] = true; ++distinct_slots; }
        }
        for (int i = 0; i < created; ++i) {
            for (int j = i + 1; j < created; ++j) {
                expect(pids[i] != pids[j], "two live tasks share a pid");
            }
        }

        expect(await_zombies(pids, created, 100), "children never reached Zombie");
        check_invariants();
        reap_and_verify(pids, created, true);
    }

    expect(distinct_slots <= BATCH + 4, "slots are not being reused between batches");
    expect(count_dead_slots() == dead_before, "task slots leaked");
    expect(settle_heap(heap_before), "kernel stacks leaked on the heap");
}

// ---- test 2: repeated sleep / wake ---------------------------------------
// State path: Ready -> Running -> Sleeping -> Ready -> ... -> Zombie -> Dead.

constexpr int SLEEPERS = 6;
constexpr uint64_t LONG_SLEEP_TICKS = 24;
constexpr uint64_t WAKE_LATE_SLACK = QUANTUM_TICKS * (SLEEPERS + 6);

volatile int g_sleep_rounds = 1;
volatile uint32_t g_sleep_early[MAX_SLOTS];
volatile uint32_t g_sleep_late[MAX_SLOTS];
volatile uint32_t g_sleep_done[MAX_SLOTS];

uint64_t sleep_duration(int slot, int round) {
    return static_cast<uint64_t>((slot * 7 + round * 3) % 7);
}

void sleeper_entry() {
    const int slot = self_slot();
    for (int round = 0; round < g_sleep_rounds; ++round) {
        const uint64_t d = sleep_duration(slot, round);
        const uint64_t start = interrupts::ticks();
        sleep_current(d);
        const uint64_t woke = interrupts::ticks();
        if (woke < start + d) g_sleep_early[slot] = g_sleep_early[slot] + 1;
        if (woke > start + d + WAKE_LATE_SLACK) g_sleep_late[slot] = g_sleep_late[slot] + 1;
        g_sleep_done[slot] = g_sleep_done[slot] + 1;
    }
    exit_current(slot);
}

void long_sleeper_entry() {
    const int slot = self_slot();
    const uint64_t start = interrupts::ticks();
    sleep_current(LONG_SLEEP_TICKS);
    const uint64_t woke = interrupts::ticks();
    if (woke < start + LONG_SLEEP_TICKS) g_sleep_early[slot] = g_sleep_early[slot] + 1;
    if (woke > start + LONG_SLEEP_TICKS + WAKE_LATE_SLACK) g_sleep_late[slot] = g_sleep_late[slot] + 1;
    g_sleep_done[slot] = g_sleep_done[slot] + 1;
    exit_current(slot);
}

void test_sleep_wake() {
    const int rounds = g_params.sleep_rounds;
    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();

    for (int i = 0; i < MAX_SLOTS; ++i) {
        g_sleep_early[i] = 0;
        g_sleep_late[i] = 0;
        g_sleep_done[i] = 0;
    }
    g_sleep_rounds = rounds;

    Pid pids[SLEEPERS + 1];
    int created = 0;
    Task* long_sleeper = create(&long_sleeper_entry, "stress-long-sleep");
    if (!expect(long_sleeper != nullptr, "create() failed for the long sleeper")) return;
    const Pid long_pid = long_sleeper->pid;
    pids[created++] = long_pid;
    for (int i = 0; i < SLEEPERS; ++i) {
        Task* t = create(&sleeper_entry, "stress-sleep");
        if (!expect(t != nullptr, "create() failed for a sleeper")) break;
        pids[created++] = t->pid;
    }

    // While the long sleeper is asleep we must be able to see it Sleeping,
    // with a wake time still in the future.
    bool saw_sleeping = false;
    const uint64_t watch_deadline = interrupts::ticks() + LONG_SLEEP_TICKS / 2;
    while (!saw_sleeping && interrupts::ticks() < watch_deadline) {
        {
            IrqGuard guard;
            Task* t = find_by_pid(long_pid);
            if (t != nullptr && t->state == State::Sleeping && t->wake_tick > interrupts::ticks()) {
                saw_sleeping = true;
            }
        }
        if (!saw_sleeping) scheduler::yield();
    }
    expect(saw_sleeping, "a sleeping task never showed State::Sleeping with a future wake tick");

    const bool all_done = await_zombies(pids, created, LONG_SLEEP_TICKS + WAKE_LATE_SLACK * (rounds + 2));
    expect(all_done, "a sleeper never woke up (lost wake-up)");
    if (all_done) {
        check_invariants();
        reap_and_verify(pids, created, true);
        for (int i = 0; i < created; ++i) {
            const int slot = pid_slot_of(pids[i]);
            const uint32_t expected_rounds = (pids[i] == long_pid) ? 1u : static_cast<uint32_t>(rounds);
            expect(g_sleep_early[slot] == 0, "a task woke before its sleep time elapsed");
            expect(g_sleep_late[slot] == 0, "a task woke absurdly late");
            expect(g_sleep_done[slot] == expected_rounds, "a sleeper did not complete all its sleep rounds");
        }
    }

    expect(count_dead_slots() == dead_before, "task slots leaked after sleeping tasks");
    expect(settle_heap(heap_before), "heap leaked after sleeping tasks");
}

// ---- test 3: repeated block / unblock, stale pids ------------------------
// State path: Ready -> Running -> Blocked -> Ready -> ... -> Zombie -> Dead.
// The unblock() calls are only made at moments the target is observed to be
// Blocked, and a stale pid must never wake a slot's new owner.

constexpr int BLOCKERS = 8;
volatile int g_block_rounds = 1;
volatile uint32_t g_block_progress[MAX_SLOTS];
volatile bool g_occupant_release = false;

void blocker_entry() {
    const int slot = self_slot();
    for (int round = 0; round < g_block_rounds; ++round) {
        g_block_progress[slot] = g_block_progress[slot] + 1;
        block_current();
    }
    exit_current(slot);
}

// Parks in Blocked until told to leave, so the slot can be reclaimed again.
void occupant_entry() {
    while (!g_occupant_release) block_current();
    exit_current(self_slot());
}

bool all_blocked_at(const Pid* pids, int count, uint32_t progress) {
    for (int i = 0; i < count; ++i) {
        Task* t = find_by_pid(pids[i]);
        if (t == nullptr || t->state != State::Blocked) return false;
        if (g_block_progress[pid_slot_of(pids[i])] != progress) return false;
    }
    return true;
}

void test_block_unblock() {
    const int rounds = g_params.block_rounds;
    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();

    for (int i = 0; i < MAX_SLOTS; ++i) g_block_progress[i] = 0;
    g_block_rounds = rounds;

    Pid pids[BLOCKERS];
    int created = 0;
    for (int i = 0; i < BLOCKERS; ++i) {
        Task* t = create(&blocker_entry, "stress-block");
        if (!expect(t != nullptr, "create() failed for a blocker")) break;
        pids[created++] = t->pid;
    }

    for (int round = 1; round <= rounds; ++round) {
        bool reached = false;
        const uint64_t deadline = interrupts::ticks() + 100;
        while (interrupts::ticks() <= deadline) {
            if (all_blocked_at(pids, created, static_cast<uint32_t>(round))) { reached = true; break; }
            scheduler::yield();
        }
        if (!expect(reached, "blockers did not all reach Blocked")) break;

        check_invariants();

        {
            IrqGuard guard;

            // unblock() must only affect Blocked tasks: not the running
            // caller, not NO_PID.
            Task* self = current();
            unblock(self->pid);
            expect(self->state == State::Running, "unblock() changed a Running task");
            unblock(NO_PID);

            for (int i = 0; i < created; ++i) unblock(pids[i]);
            for (int i = 0; i < created; ++i) {
                Task* t = find_by_pid(pids[i]);
                expect(t != nullptr && t->state == State::Ready, "unblock() did not make a Blocked task Ready");
            }
            // A second unblock on an already-Ready task must be harmless.
            for (int i = 0; i < created; ++i) unblock(pids[i]);
            for (int i = 0; i < created; ++i) {
                Task* t = find_by_pid(pids[i]);
                expect(t != nullptr && t->state == State::Ready, "a repeated unblock() changed a Ready task");
            }
        }
    }

    expect(await_zombies(pids, created, 200), "blockers never exited after the last unblock");
    const Pid stale_pid = (created > 0) ? pids[0] : NO_PID;
    reap_and_verify(pids, created, true);
    for (int i = 0; i < created; ++i) {
        expect(g_block_progress[pid_slot_of(pids[i])] == static_cast<uint32_t>(rounds),
               "a blocker did not complete every block/unblock round");
    }

    // Stale-pid safety: the slot of `stale_pid` is free again. unblock() with
    // the old pid must not wake the slot's next owner.
    if (stale_pid != NO_PID) {
        g_occupant_release = false;
        Task* occupant = nullptr;
        {
            IrqGuard guard;
            occupant = create(&occupant_entry, "stress-occupant");
        }
        if (expect(occupant != nullptr, "create() failed for the slot occupant")) {
            const Pid occupant_pid = occupant->pid;
            bool blocked = false;
            const uint64_t deadline = interrupts::ticks() + 100;
            while (interrupts::ticks() <= deadline) {
                Task* t = find_by_pid(occupant_pid);
                if (t != nullptr && t->state == State::Blocked) { blocked = true; break; }
                scheduler::yield();
            }
            expect(blocked, "occupant task never blocked");
            if (blocked) {
                if (pid_slot_of(occupant_pid) == pid_slot_of(stale_pid)) {
                    expect(occupant_pid != stale_pid, "slot reuse produced the same pid");
                    IrqGuard guard;
                    unblock(stale_pid);
                    Task* t = find_by_pid(occupant_pid);
                    expect(t != nullptr && t->state == State::Blocked,
                           "a stale pid woke the new owner of its slot");
                }
                g_occupant_release = true;
                unblock(occupant_pid);
            }
            // Always reclaim the occupant so the slot and stack are not lost.
            // (If it never blocked it is Ready or Running and will see the flag.)
            g_occupant_release = true;
            expect(await_zombies(&occupant_pid, 1, 100), "occupant never exited");
            reap_and_verify(&occupant_pid, 1, true);
        }
    }

    expect(count_dead_slots() == dead_before, "task slots leaked after block/unblock");
    expect(settle_heap(heap_before), "heap leaked after block/unblock");
}

// ---- test 4: timer preemption of CPU-bound tasks -------------------------
// The spinners never yield. If the timer tick did not preempt them the runner
// (sleeping, then Ready) would never run again and the boot would hang, which
// is why the caller of stress_test() gives up on a deadline.

volatile bool g_spin_stop = false;
volatile uint64_t g_spin_count[MAX_SLOTS];

void spinner_entry() {
    const int slot = self_slot();
    while (!g_spin_stop) g_spin_count[slot] = g_spin_count[slot] + 1;
    exit_current(slot);
}

void test_preemption() {
    const int n = g_params.spinners;
    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();

    g_spin_stop = false;
    for (int i = 0; i < MAX_SLOTS; ++i) g_spin_count[i] = 0;

    Pid pids[MAX_SLOTS];
    uint64_t last[MAX_SLOTS] = {};
    int created = 0;
    for (int i = 0; i < n; ++i) {
        Task* t = create(&spinner_entry, "stress-spin");
        if (!expect(t != nullptr, "create() failed for a spinner")) break;
        pids[created++] = t->pid;
    }

    // Long enough that every spinner is guaranteed at least one full quantum.
    const uint64_t window = QUANTUM_TICKS * (static_cast<uint64_t>(n) + 6);
    for (int w = 0; w < g_params.spin_windows; ++w) {
        for (int i = 0; i < created; ++i) last[i] = g_spin_count[pid_slot_of(pids[i])];

        const uint64_t start = interrupts::ticks();
        sleep_current(window);
        expect(interrupts::ticks() >= start + window, "the runner woke before its sleep time elapsed");

        {
            IrqGuard guard;
            for (int i = 0; i < created; ++i) {
                Task* t = find_by_pid(pids[i]);
                // We are the Running task, so a spinner can only be Ready:
                // preempted, not blocked, sleeping or exited.
                expect(t != nullptr && t->state == State::Ready, "a CPU-bound task is not Ready while preempted");
                expect(g_spin_count[pid_slot_of(pids[i])] > last[i],
                       "a CPU-bound task made no progress in a full window (starved)");
            }
        }
        check_invariants();
    }

    g_spin_stop = true;
    expect(await_zombies(pids, created, window * 2), "spinners did not exit after being told to stop");
    reap_and_verify(pids, created, true);

    expect(count_dead_slots() == dead_before, "task slots leaked after the spinners");
    expect(settle_heap(heap_before), "heap leaked after the spinners");
}

// ---- test 5: task-table exhaustion, repeated -----------------------------
// Fill every free slot, check create() fails cleanly at the limit, drain,
// check every slot comes back with a newer generation and nothing leaked.

void test_exhaustion() {
    Pid pids[MAX_SLOTS];
    uint64_t gens[MAX_SLOTS];

    for (int round = 0; round < g_params.exhaust_rounds; ++round) {
        const size_t heap_before = heap::bytes_in_use();
        const int dead_before = count_dead_slots();
        for (int i = 0; i < capacity(); ++i) gens[i] = at(i)->generation;

        int created = 0;
        bool hit_limit = false;
        for (int attempt = 0; attempt <= dead_before; ++attempt) {
            IrqGuard guard;
            Task* t = create(&exit_with_slot_entry, "stress-fill");
            if (t == nullptr) {
                hit_limit = true;
                expect(count_dead_slots() == 0, "create() failed while a free slot was still available");
                break;
            }
            pids[created++] = t->pid;
            const int slot = pid_slot_of(t->pid);
            expect(t->state == State::Ready, "task created at the table limit is not Ready");
            expect(pid_generation_of(t->pid) == gens[slot] + 1, "slot generation did not advance by one");
        }
        expect(hit_limit, "create() kept succeeding past the size of the task table");
        expect(created == dead_before, "created a different number of tasks than there were free slots");

        expect(await_zombies(pids, created, 400), "filler tasks never all exited");
        check_invariants();
        reap_and_verify(pids, created, true);

        expect(count_dead_slots() == dead_before, "task slots leaked after exhaustion");
        expect(settle_heap(heap_before), "kernel stacks leaked after exhaustion");
    }
}

// ---- test 6: private address spaces, repeated ----------------------------
// Processes (tasks with their own CR3) that all map the SAME virtual address
// to DIFFERENT frames, and sleep between writing and reading it back, so any
// missed CR3 switch shows up as one process reading another's data. Then check
// that exiting returns every frame of the address space.

constexpr uintptr_t PROC_TEST_VIRT = 0x40100000;
constexpr int PROC_BATCH = 4;
constexpr uint32_t PROC_CHECKS = 3;
constexpr uint64_t FRAMES_PER_TEST_PROCESS = 5;   // PML4, PDPT, PD, PT, one data page

volatile uint32_t g_proc_ok[MAX_SLOTS];
volatile uint32_t g_proc_bad[MAX_SLOTS];

void process_entry_fn() {
    const int slot = self_slot();
    volatile uint64_t* cell = reinterpret_cast<volatile uint64_t*>(PROC_TEST_VIRT);
    for (uint32_t i = 0; i < PROC_CHECKS; ++i) {
        const uint64_t value = (static_cast<uint64_t>(slot) << 32) | (0xC0DE0000u + i);
        *cell = value;
        sleep_current(1);          // other address spaces run while we are away
        if (*cell == value) {
            g_proc_ok[slot] = g_proc_ok[slot] + 1;
        } else {
            g_proc_bad[slot] = g_proc_bad[slot] + 1;
        }
    }
    exit_current(slot);
}

// Builds an address space with one private data page and starts a task in it.
// Returns the pid, or NO_PID after undoing everything on failure.
Pid spawn_test_process() {
    IrqGuard guard;

    const uint64_t frames_before = pmm::used_frames();

    const uint64_t cr3 = paging::create_address_space();
    if (!expect(cr3 != 0, "create_address_space() failed")) return NO_PID;

    const uintptr_t frame = pmm::alloc_frame();
    if (!expect(frame != 0, "out of physical frames for a process page")) {
        paging::destroy_address_space(cr3);
        return NO_PID;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(frame);
    for (size_t i = 0; i < pmm::FRAME_SIZE; ++i) bytes[i] = 0;

    const uint64_t flags = paging::PAGE_PRESENT | paging::PAGE_WRITABLE | paging::PAGE_USER |
                           paging::PAGE_OWNED | paging::PAGE_NX;
    if (!expect(paging::map_page_in(cr3, PROC_TEST_VIRT, frame, flags), "map_page_in() failed")) {
        pmm::free_frame(frame);
        paging::destroy_address_space(cr3);
        return NO_PID;
    }
    expect(pmm::used_frames() - frames_before == FRAMES_PER_TEST_PROCESS,
           "building an address space used an unexpected number of frames");

    Task* t = create_process(&process_entry_fn, "stress-proc", cr3, 0, 0);
    if (!expect(t != nullptr, "create_process() failed")) {
        paging::destroy_address_space(cr3);   // frees the OWNED data page too
        return NO_PID;
    }
    expect(t->cr3 == cr3 && t->state == State::Ready, "a new process has the wrong CR3 or state");
    return t->pid;
}

void test_address_spaces() {
    for (int round = 0; round < g_params.proc_rounds; ++round) {
        const size_t heap_before = heap::bytes_in_use();
        const int dead_before = count_dead_slots();
        const uint64_t frames_before = pmm::used_frames();

        for (int i = 0; i < MAX_SLOTS; ++i) { g_proc_ok[i] = 0; g_proc_bad[i] = 0; }

        Pid pids[PROC_BATCH];
        int created = 0;
        for (int i = 0; i < PROC_BATCH; ++i) {
            const Pid pid = spawn_test_process();
            if (pid == NO_PID) break;
            pids[created++] = pid;
        }
        expect(created == PROC_BATCH, "could not start every test process");

        // Each live process must hold its own, distinct address space.
        {
            IrqGuard guard;
            for (int i = 0; i < created; ++i) {
                Task* a = find_by_pid(pids[i]);
                if (a == nullptr) continue;
                for (int j = i + 1; j < created; ++j) {
                    Task* b = find_by_pid(pids[j]);
                    if (b != nullptr) expect(a->cr3 != b->cr3, "two processes share an address space");
                }
            }
        }

        expect(await_zombies(pids, created, 200), "test processes never exited");
        check_invariants();
        reap_and_verify(pids, created, true);

        for (int i = 0; i < created; ++i) {
            const int slot = pid_slot_of(pids[i]);
            expect(g_proc_bad[slot] == 0, "a process saw another address space's data (CR3 not switched)");
            expect(g_proc_ok[slot] == PROC_CHECKS, "a process did not finish all its memory checks");
        }

        expect(count_dead_slots() == dead_before, "task slots leaked after processes");
        expect(settle_heap(heap_before), "kernel stacks leaked after processes");
        // Round 0 may still grow the heap for the new stacks; afterwards the
        // same workload must return every frame, address-space frames included.
        if (round > 0) {
            expect(settle_frames(frames_before), "physical frames leaked after processes exited");
        }
    }
}

// ---- test 7: orphan adoption, repeated -----------------------------------
// A parent that exits with live children must hand them to init, and init must
// reap them, so the slots and stacks of the whole family come back.

constexpr int ORPHANS = 3;
volatile Pid g_orphan_pids[ORPHANS];
volatile int g_orphan_created = 0;

void orphan_child_entry() {
    const int slot = self_slot();
    sleep_current(4 + static_cast<uint64_t>(slot % 3));
    exit_current(slot);
}

void orphan_parent_entry() {
    int made = 0;
    for (int i = 0; i < ORPHANS; ++i) {
        Task* t = create(&orphan_child_entry, "stress-orphan");
        if (t == nullptr) break;
        g_orphan_pids[made++] = t->pid;
    }
    g_orphan_created = made;
    exit_current(made);
}

void test_orphan_adoption() {
    for (int round = 0; round < g_params.orphan_rounds; ++round) {
        const size_t heap_before = heap::bytes_in_use();
        const int dead_before = count_dead_slots();

        Task* init_task = find_init();
        if (!expect(init_task != nullptr, "the init task is missing")) return;

        for (int i = 0; i < ORPHANS; ++i) g_orphan_pids[i] = NO_PID;
        g_orphan_created = 0;

        Task* parent = create(&orphan_parent_entry, "stress-orphan-parent");
        if (!expect(parent != nullptr, "create() failed for the orphan parent")) return;
        Pid parent_pid = parent->pid;

        expect(await_zombies(&parent_pid, 1, 200), "the orphan parent never exited");
        expect(g_orphan_created == ORPHANS, "the orphan parent could not create all its children");

        // The children were handed to init the moment the parent exited.
        // (A child may already be gone if init reaped it in the meantime.)
        for (int i = 0; i < g_orphan_created; ++i) {
            IrqGuard guard;
            Task* child = find_by_pid(g_orphan_pids[i]);
            if (child == nullptr) continue;
            expect(child->parent_pid == init_task->pid, "an orphan was not adopted by init");
            expect(child_list_has(init_task, child->pid), "an adopted orphan is missing from init's child list");
        }
        {
            Task* p = find_by_pid(parent_pid);
            expect(p != nullptr && p->exit_code == ORPHANS, "the orphan parent's exit code was lost");
        }
        reap_and_verify(&parent_pid, 1, false);

        // Init, not us, must now reap the orphans.
        bool all_gone = false;
        const uint64_t deadline = interrupts::ticks() + 200;
        while (!all_gone && interrupts::ticks() < deadline) {
            all_gone = true;
            for (int i = 0; i < g_orphan_created; ++i) {
                if (find_by_pid(g_orphan_pids[i]) != nullptr) all_gone = false;
            }
            if (!all_gone) sleep_current(2);
        }
        expect(all_gone, "adopted orphans were never reaped by init");

        check_invariants();
        expect(count_dead_slots() == dead_before, "task slots leaked after orphans");
        expect(settle_heap(heap_before), "kernel stacks leaked after orphans");
    }
}

// ---- test 8: seeded random churn ----------------------------------------
// A long, repeatable random mix of create / sleep / block / unblock / reap /
// yield, with every invariant checked after every single operation.

constexpr int CHURN_MAX_LIVE = 20;
enum : uint8_t { ChurnExiter = 0, ChurnSleeper = 1, ChurnBlocker = 2 };

struct Live {
    Pid pid;
    uint8_t kind;
};

volatile uint32_t g_churn_release[MAX_SLOTS];
uint64_t g_rng = 0x2545F4914F6CDD1Dull;

uint32_t next_random() {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(g_rng >> 33);
}

void churn_exit_entry() {
    exit_current(self_slot());
}

void churn_sleep_entry() {
    const int slot = self_slot();
    sleep_current(1 + static_cast<uint64_t>(slot % 5));
    exit_current(slot);
}

void churn_block_entry() {
    const int slot = self_slot();
    while (!g_churn_release[slot]) block_current();
    g_churn_release[slot] = 0;
    exit_current(slot);
}

// Reaps one finished child if there is one. Never blocks.
bool churn_reap_one(Live* live, int& n) {
    bool zombie_present = false;
    {
        IrqGuard guard;
        for (int i = 0; i < n; ++i) {
            Task* t = find_by_pid(live[i].pid);
            if (t != nullptr && t->state == State::Zombie) zombie_present = true;
        }
    }
    if (!zombie_present) return false;

    Pid pid = NO_PID;
    int code = 0;
    if (!expect(wait_for_child(&pid, &code), "wait_for_child failed with a Zombie child present")) return false;

    int index = -1;
    for (int i = 0; i < n; ++i) {
        if (live[i].pid == pid) index = i;
    }
    if (expect(index >= 0, "reaped a pid the churn test did not create")) {
        expect(code == pid_slot_of(pid), "a churn child's exit code was lost or wrong");
        live[index] = live[n - 1];
        --n;
    }
    expect(find_by_pid(pid) == nullptr, "a reaped churn child still resolves");
    return true;
}

// Wakes every blocker that is observed Blocked. Returns how many it woke.
int churn_release_blocked(Live* live, int n, int max_to_release) {
    int released = 0;
    IrqGuard guard;
    for (int i = 0; i < n && released < max_to_release; ++i) {
        if (live[i].kind != ChurnBlocker) continue;
        Task* t = find_by_pid(live[i].pid);
        if (t == nullptr || t->state != State::Blocked) continue;
        g_churn_release[pid_slot_of(t->pid)] = 1;
        unblock(t->pid);
        ++released;
    }
    return released;
}

void churn_verify(const Live* live, int n) {
    {
        IrqGuard guard;
        Task* self = current();

        int children = 0;
        for (int i = 0; i < capacity(); ++i) {
            Task* t = at(i);
            if (t->state != State::Dead && t->parent_pid == self->pid) ++children;
        }
        expect(children == n, "the number of children differs from what the churn test spawned and not yet reaped");

        for (int i = 0; i < n; ++i) {
            Task* t = find_by_pid(live[i].pid);
            if (!expect(t != nullptr, "a churn child vanished before being reaped")) continue;
            const State s = t->state;
            bool allowed = (s == State::Ready) || (s == State::Zombie);
            if (live[i].kind == ChurnSleeper) allowed = allowed || (s == State::Sleeping);
            if (live[i].kind == ChurnBlocker) allowed = allowed || (s == State::Blocked);
            expect(allowed, "a churn child is in a state its behaviour can never produce");
        }
    }
    check_invariants();
}

void test_churn() {
    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();
    for (int i = 0; i < MAX_SLOTS; ++i) g_churn_release[i] = 0;

    Live live[CHURN_MAX_LIVE];
    int n = 0;
    uint32_t spawned = 0;
    uint32_t reaped = 0;

    for (int op = 0; op < g_params.churn_ops; ++op) {
        const uint32_t r = next_random();
        const uint32_t pick = r % 8;

        if (pick <= 2) {
            if (n < CHURN_MAX_LIVE) {
                const uint8_t kind = static_cast<uint8_t>((r >> 8) % 3);
                EntryFn entry = (kind == ChurnExiter) ? &churn_exit_entry
                              : (kind == ChurnSleeper) ? &churn_sleep_entry
                              : &churn_block_entry;
                IrqGuard guard;
                Task* t = create(entry, "stress-churn");
                if (expect(t != nullptr, "create() failed with a free slot available")) {
                    g_churn_release[pid_slot_of(t->pid)] = 0;
                    live[n].pid = t->pid;
                    live[n].kind = kind;
                    ++n;
                    ++spawned;
                }
            }
        } else if (pick <= 4) {
            if (churn_reap_one(live, n)) ++reaped;
        } else if (pick == 5) {
            churn_release_blocked(live, n, 1);
        } else if (pick == 6) {
            scheduler::yield();
        } else {
            sleep_current(1);
        }

        churn_verify(live, n);
    }

    // Drain: wake every blocker once it is Blocked, reap everything.
    const uint64_t deadline = interrupts::ticks() + 1200;
    while (n > 0 && interrupts::ticks() < deadline) {
        churn_release_blocked(live, n, n);
        while (churn_reap_one(live, n)) ++reaped;
        if (n > 0) scheduler::yield();
    }
    expect(n == 0, "churn children never all finished");
    expect(spawned == reaped, "spawned and reaped counts differ");

    check_invariants();
    expect(count_dead_slots() == dead_before, "task slots leaked after churn");
    expect(settle_heap(heap_before), "kernel stacks leaked after churn");
}

// ---- test 9: kernel heap under preemption --------------------------------
// Tasks allocate, fill, verify and free random-sized blocks for several
// quanta, so the timer preempts them in the middle of kmalloc()/kfree() while
// other tasks use the same free list. A block handed out twice, a corrupted
// free list or a lost block shows up as a bad pattern, a failed allocation or
// a heap that does not return to its starting size.

constexpr int HAMMERS = 4;
constexpr int HAMMER_BLOCKS = 12;
volatile uint32_t g_hammer_bad[MAX_SLOTS];
volatile uint32_t g_hammer_done[MAX_SLOTS];
volatile uint64_t g_hammer_end_tick = 0;
volatile int g_hammer_min_rounds = 0;

bool block_has_pattern(const uint8_t* block, size_t size, uint8_t tag) {
    for (size_t i = 0; i < size; ++i) {
        if (block[i] != tag) return false;
    }
    return true;
}

void hammer_entry() {
    const int slot = self_slot();
    uint32_t seed = 0x9E3779B9u * (static_cast<uint32_t>(slot) + 1);
    uint8_t* blocks[HAMMER_BLOCKS] = {};
    size_t sizes[HAMMER_BLOCKS] = {};
    uint8_t tags[HAMMER_BLOCKS] = {};
    uint32_t bad = 0;

    for (int round = 0; round < g_hammer_min_rounds || interrupts::ticks() < g_hammer_end_tick; ++round) {
        for (int i = 0; i < HAMMER_BLOCKS; ++i) {
            seed = seed * 1664525u + 1013904223u;
            if (blocks[i] != nullptr) {
                if (!block_has_pattern(blocks[i], sizes[i], tags[i])) ++bad;
                heap::kfree(blocks[i]);
                blocks[i] = nullptr;
            } else {
                const size_t size = 16 + (seed >> 16) % 1500;
                uint8_t* block = static_cast<uint8_t*>(heap::kmalloc(size));
                if (block == nullptr) { ++bad; continue; }
                const uint8_t tag = static_cast<uint8_t>(slot * 31 + i * 7 + round);
                for (size_t b = 0; b < size; ++b) block[b] = tag;
                blocks[i] = block;
                sizes[i] = size;
                tags[i] = tag;
            }
        }
    }
    for (int i = 0; i < HAMMER_BLOCKS; ++i) {
        if (blocks[i] == nullptr) continue;
        if (!block_has_pattern(blocks[i], sizes[i], tags[i])) ++bad;
        heap::kfree(blocks[i]);
    }
    g_hammer_bad[slot] = bad;
    g_hammer_done[slot] = 1;
    exit_current(slot);
}

void test_heap_under_preemption() {
    const size_t heap_before = heap::bytes_in_use();
    const int dead_before = count_dead_slots();

    for (int i = 0; i < MAX_SLOTS; ++i) { g_hammer_bad[i] = 0; g_hammer_done[i] = 0; }
    g_hammer_min_rounds = g_params.heap_rounds;
    g_hammer_end_tick = interrupts::ticks() + static_cast<uint64_t>(g_params.heap_ticks);

    Pid pids[HAMMERS];
    int created = 0;
    for (int i = 0; i < HAMMERS; ++i) {
        Task* t = create(&hammer_entry, "stress-heap");
        if (!expect(t != nullptr, "create() failed for a heap task")) break;
        pids[created++] = t->pid;
    }

    const uint64_t timeout = static_cast<uint64_t>(g_params.heap_ticks) * 4 + QUANTUM_TICKS * 40;
    const bool all_done = await_zombies(pids, created, timeout);
    expect(all_done, "heap tasks never finished");
    if (all_done) {
        check_invariants();
        reap_and_verify(pids, created, true);
        for (int i = 0; i < created; ++i) {
            const int slot = pid_slot_of(pids[i]);
            expect(g_hammer_done[slot] == 1, "a heap task did not run to completion");
            expect(g_hammer_bad[slot] == 0, "heap blocks were corrupted, duplicated or could not be allocated");
        }
    }

    expect(count_dead_slots() == dead_before, "task slots leaked after the heap test");
    expect(settle_heap(heap_before), "heap bytes leaked or lost under preemption");
}

// ---- runner ---------------------------------------------------------------

void run_test(const char* name, void (*test)()) {
    g_test_name = name;
    const int failures_before = g_test_failures;
    const uint64_t start = interrupts::ticks();

    if (g_full) {
        vga::print("  ");
        vga::print(name);
        vga::print(" ... ");
    }
    test();
    check_invariants();

    const bool ok = (g_test_failures == failures_before);
    if (g_full) {
        print_status(ok);
        vga::print(" (");
        print_uint(interrupts::ticks() - start);
        vga::print(" ticks)\n");
    }
}

void run_all_tests() {
    run_test("create/exit/reap   ", &test_create_exit_reap);
    run_test("sleep/wake         ", &test_sleep_wake);
    run_test("block/unblock      ", &test_block_unblock);
    run_test("preemption         ", &test_preemption);
    run_test("table exhaustion   ", &test_exhaustion);
    run_test("address spaces     ", &test_address_spaces);
    run_test("orphan adoption    ", &test_orphan_adoption);
    run_test("random churn       ", &test_churn);
    run_test("heap + preemption  ", &test_heap_under_preemption);
}

void runner_entry() {
    run_all_tests();
    g_runner_ok = (g_test_failures == 0);
    g_runner_done = true;
    exit_current(g_runner_ok ? 0 : 1);
}

}

bool stress_test(bool full) {
    g_full = full;
    g_params = full ? FULL : QUICK;
    g_test_failures = 0;
    g_failure_lines = 0;
    g_test_name = "";
    g_runner_done = false;
    g_runner_ok = false;

    vga::print(full ? "Scheduler stress test (full):\n" : "Scheduler stress test (quick): ");

    if (capacity() > MAX_SLOTS) {
        print_status(false);
        vga::print(" - the stress test arrays are smaller than the task table\n");
        return false;
    }

    const uint64_t start = interrupts::ticks();
    const size_t heap_before = heap::bytes_in_use();

    Task* runner = create(&runner_entry, "stress-runner");
    if (runner == nullptr) {
        print_status(false);
        vga::print(" - could not create the stress runner task\n");
        return false;
    }
    const Pid runner_pid = runner->pid;

    // Every wait below has a deadline, so lost wake-ups, livelocks and tasks
    // that never finish are reported as a failure instead of a hang. (It
    // cannot help if the timer stops preempting altogether: the caller is
    // then starved as well.)
    const uint64_t deadline = start + (full ? 4000 : 1200);
    for (;;) {
        Task* t = find_by_pid(runner_pid);
        if (t == nullptr || t->state == State::Zombie) break;
        if (interrupts::ticks() > deadline) {
            print_status(false);
            vga::print(" - the stress runner did not finish in time (hung or starved)\n");
            return false;
        }
        sleep_current(2);
    }

    // Reap the runner. Anything else that happens to be a finished child of
    // ours is reaped on the way, which is harmless.
    for (int attempt = 0; attempt < MAX_SLOTS; ++attempt) {
        Task* t = find_by_pid(runner_pid);
        if (t == nullptr) break;
        Pid pid = NO_PID;
        int code = 0;
        if (!wait_for_child(&pid, &code)) break;
    }

    bool ok = g_runner_done && g_runner_ok;
    if (!settle_heap(heap_before)) {
        ++g_test_failures;
        ok = false;
        vga::print("\n  FAIL: the stress runner's own stack was not returned to the heap");
    }

    if (full) vga::print("  Result: ");
    print_status(ok);
    vga::print(" (");
    print_uint(interrupts::ticks() - start);
    vga::print(" ticks");
    if (!ok) {
        vga::print(", ");
        print_uint(static_cast<uint64_t>(g_test_failures));
        vga::print(" failed check(s)");
    }
    vga::print(")\n");
    return ok;
}

}
