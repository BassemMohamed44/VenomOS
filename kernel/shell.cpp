#include "shell.hpp"

#include "fs.hpp"
#include "heap.hpp"
#include "pmm.hpp"
#include "process.hpp"
#include "ring3.hpp"
#include "snake.hpp"
#include "task.hpp"
#include "vga.hpp"

namespace {

constexpr unsigned int COMMAND_CAPACITY = 80;
char command[COMMAND_CAPACITY] = {};
unsigned int command_length = 0;

bool token_is(const char* full, size_t token_len, const char* word) {
    for (size_t i = 0; i < token_len; ++i) {
        if (word[i] == '\0' || full[i] != word[i]) return false;
    }
    return word[token_len] == '\0';
}

void print_uint(uint64_t value) {
    char digits[21];
    int index = 20;
    digits[index] = '\0';

    if (value == 0) {
        digits[--index] = '0';
    } else {
        while (value != 0) {
            digits[--index] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }

    vga::print(&digits[index]);
}


void print_int(int value) {
    if (value < 0) {
        vga::put_char('-');
        print_uint(static_cast<uint64_t>(-static_cast<int64_t>(value)));
    } else {
        print_uint(static_cast<uint64_t>(value));
    }
}

void prompt() {
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::print("VenomOS> ");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

const char* state_name(task::State state) {
    switch (state) {
        case task::State::Dead:     return "DEAD";
        case task::State::New:      return "NEW";
        case task::State::Ready:    return "READY";
        case task::State::Running:  return "RUNNING";
        case task::State::Blocked:  return "BLOCKED";
        case task::State::Sleeping: return "SLEEPING";
        case task::State::Zombie:   return "ZOMBIE";
    }
    return "?";
}

bool run_memtest() {
    constexpr int BLOCK_COUNT = 5;
    constexpr size_t sizes[BLOCK_COUNT] = {16, 200, 4100, 64, 1024};
    void* blocks[BLOCK_COUNT] = {};

    bool ok = true;

    for (int i = 0; i < BLOCK_COUNT && ok; ++i) {
        blocks[i] = heap::kmalloc(sizes[i]);
        if (blocks[i] == nullptr) { ok = false; break; }

        uint8_t pattern = static_cast<uint8_t>(0xA0 + i);
        uint8_t* bytes = reinterpret_cast<uint8_t*>(blocks[i]);
        for (size_t b = 0; b < sizes[i]; ++b) bytes[b] = pattern;
        for (size_t b = 0; b < sizes[i]; ++b) {
            if (bytes[b] != pattern) { ok = false; break; }
        }
    }

    for (int i = 0; i < BLOCK_COUNT; ++i) {
        if (blocks[i] != nullptr) heap::kfree(blocks[i]);
    }

    return ok;
}


void demo_task_counter_a() {
    for (uint32_t i = 0; i < 100; ++i) {
        vga::put_char_at(20, 0, 'A', vga::Color::LightRed);
        vga::put_char_at(20, 2, static_cast<char>('0' + (i % 10)), vga::Color::LightRed);
        vga::put_char_at(20, 4, static_cast<char>('0' + ((i / 10) % 10)), vga::Color::LightRed);
        for (volatile uint32_t spin = 0; spin < 3000000; ++spin) {}
    }
    vga::put_char_at(20, 6, '(', vga::Color::DarkGrey);
    vga::put_char_at(20, 7, 'd', vga::Color::DarkGrey);
    vga::put_char_at(20, 8, 'o', vga::Color::DarkGrey);
    vga::put_char_at(20, 9, 'n', vga::Color::DarkGrey);
    vga::put_char_at(20, 10, 'e', vga::Color::DarkGrey);
    vga::put_char_at(20, 11, ')', vga::Color::DarkGrey);
}

void demo_task_counter_b() {
    for (uint32_t i = 0; i < 100; ++i) {
        vga::put_char_at(21, 0, 'B', vga::Color::LightBlue);
        vga::put_char_at(21, 2, static_cast<char>('0' + (i % 10)), vga::Color::LightBlue);
        vga::put_char_at(21, 4, static_cast<char>('0' + ((i / 10) % 10)), vga::Color::LightBlue);
        for (volatile uint32_t spin = 0; spin < 3000000; ++spin) {}
    }
    vga::put_char_at(21, 6, '(', vga::Color::DarkGrey);
    vga::put_char_at(21, 7, 'd', vga::Color::DarkGrey);
    vga::put_char_at(21, 8, 'o', vga::Color::DarkGrey);
    vga::put_char_at(21, 9, 'n', vga::Color::DarkGrey);
    vga::put_char_at(21, 10, 'e', vga::Color::DarkGrey);
    vga::put_char_at(21, 11, ')', vga::Color::DarkGrey);
}

void execute() {
    command[command_length] = '\0';

    if (command_length == 0) {
        return;
    }

    size_t token_len = 0;
    while (command[token_len] != '\0' && command[token_len] != ' ') ++token_len;
    const char* args = (command[token_len] == ' ') ? &command[token_len + 1] : &command[token_len];

    if (token_is(command, token_len, "help")) {
        vga::print("Commands: help, clear, version, whoami, snake, meminfo, memtest,\n");
        vga::print("          usermode, tasks, ls, cat, write, rm, run, ps, wait, sleep\n");
    } else if (token_is(command, token_len, "clear")) {
        vga::clear(vga::Color::LightGrey, vga::Color::Black);
    } else if (token_is(command, token_len, "version")) {
        vga::print("VenomOS version 0.2.0 (x86_64)\n");
    } else if (token_is(command, token_len, "whoami")) {
        vga::print("desktop-venomos\\ghost user\n");
    } else if (token_is(command, token_len, "snake")) {
        snake::start();
    } else if (token_is(command, token_len, "meminfo")) {
        vga::print("Physical frames : ");
        print_uint(pmm::used_frames());
        vga::print(" used / ");
        print_uint(pmm::free_frames());
        vga::print(" free / ");
        print_uint(pmm::total_frames());
        vga::print(" total (4KB each)\n");

        vga::print("Kernel heap     : ");
        print_uint(heap::bytes_in_use());
        vga::print(" bytes used / ");
        print_uint(heap::bytes_free());
        vga::print(" bytes free\n");
    } else if (token_is(command, token_len, "memtest")) {
        vga::print("Running kmalloc/kfree test...\n");
        if (run_memtest()) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
            vga::print("PASS: all blocks allocated, verified, and freed correctly.\n");
        } else {
            vga::set_color(vga::Color::LightRed, vga::Color::Black);
            vga::print("FAIL: memory corruption or allocation failure detected.\n");
        }
        vga::set_color(vga::Color::White, vga::Color::Black);
    } else if (token_is(command, token_len, "usermode")) {
        vga::print("Spawning the ring 3 demo as its own task. The shell keeps running -\n");
        vga::print("watch rows 20-21 disappear if 'tasks' is still going, and the ring 3\n");
        vga::print("output appear below, then control returns here when it finishes.\n");
        if (task::create(&ring3::run_demo, "ring3-demo") == nullptr) {
            vga::set_color(vga::Color::LightRed, vga::Color::Black);
            vga::print("Failed to create the ring 3 demo task (out of task slots or memory).\n");
            vga::set_color(vga::Color::White, vga::Color::Black);
        }
    } else if (token_is(command, token_len, "tasks")) {
        vga::print("Spawning two counter tasks (rows 20 and 21). The shell stays fully\n");
        vga::print("usable while they run - this is real preemptive multitasking, driven\n");
        vga::print("by the timer interrupt, not the shell politely waiting for them.\n");
        bool a_ok = (task::create(&demo_task_counter_a, "counter-a") != nullptr);
        bool b_ok = (task::create(&demo_task_counter_b, "counter-b") != nullptr);
        if (!a_ok || !b_ok) {
            vga::set_color(vga::Color::LightRed, vga::Color::Black);
            vga::print("Failed to create one or both demo tasks (out of task slots or memory).\n");
            vga::set_color(vga::Color::White, vga::Color::Black);
        }
    } else if (token_is(command, token_len, "ls")) {
        int count = 0;
        for (int i = 0; i < fs::MAX_FILES; ++i) {
            const fs::FileEntry* entry = fs::entry_at(i);
            if (entry != nullptr && entry->used) {
                vga::print(entry->name);
                vga::print("  (");
                print_uint(entry->size_bytes);
                vga::print(" bytes)\n");
                ++count;
            }
        }
        if (count == 0) {
            vga::print("(no files)\n");
        }
    } else if (token_is(command, token_len, "cat")) {
        if (args[0] == '\0') {
            vga::print("Usage: cat <filename>\n");
        } else {
            constexpr size_t CAT_BUFFER_SIZE = 4096;
            static char buffer[CAT_BUFFER_SIZE];
            size_t bytes_read = 0;
            if (fs::read(args, buffer, CAT_BUFFER_SIZE - 1, &bytes_read)) {
                buffer[bytes_read] = '\0';
                vga::print(buffer);
                vga::put_char('\n');
            } else {
                vga::print("File not found: ");
                vga::print(args);
                vga::put_char('\n');
            }
        }
    } else if (token_is(command, token_len, "write")) {
        size_t filename_len = 0;
        while (args[filename_len] != '\0' && args[filename_len] != ' ') ++filename_len;

        if (filename_len == 0) {
            vga::print("Usage: write <filename> <text>\n");
        } else {
            char filename[fs::MAX_FILENAME];
            size_t copy_len = filename_len < fs::MAX_FILENAME - 1 ? filename_len : fs::MAX_FILENAME - 1;
            for (size_t i = 0; i < copy_len; ++i) filename[i] = args[i];
            filename[copy_len] = '\0';

            const char* text = (args[filename_len] == ' ') ? &args[filename_len + 1] : &args[filename_len];
            size_t text_len = 0;
            while (text[text_len] != '\0') ++text_len;

            if (fs::write(filename, text, text_len)) {
                vga::print("Wrote ");
                print_uint(text_len);
                vga::print(" bytes to ");
                vga::print(filename);
                vga::put_char('\n');
            } else {
                vga::print("Failed to write file (file table full or not enough contiguous disk space).\n");
            }
        }
    } else if (token_is(command, token_len, "rm")) {
        if (args[0] == '\0') {
            vga::print("Usage: rm <filename>\n");
        } else if (fs::remove(args)) {
            vga::print("Removed ");
            vga::print(args);
            vga::put_char('\n');
        } else {
            vga::print("File not found: ");
            vga::print(args);
            vga::put_char('\n');
        }
    } else if (token_is(command, token_len, "run")) {
        if (args[0] == '\0') {
            vga::print("Usage: run <filename>\n");
        } else {
            vga::print("Loading ");
            vga::print(args);
            vga::print(" from VenomFS into its own isolated address space...\n");
            if (process::run(args)) {
                vga::set_color(vga::Color::LightGreen, vga::Color::Black);
                vga::print("Process queued. The shell keeps running - output appears below\n");
                vga::print("when the scheduler gives it a turn.\n");
                vga::set_color(vga::Color::White, vga::Color::Black);
            }
        }
    } else if (token_is(command, token_len, "ps")) {
        vga::print("PID  PPID STATE     NAME\n");
        for (int i = 0; i < task::capacity(); ++i) {
            const task::Task* t = task::at(i);
            if (t == nullptr || t->state == task::State::Dead) continue;

            print_uint(t->pid);
            vga::print("    ");
            print_uint(t->parent_pid);
            vga::print("    ");
            vga::print(state_name(t->state));
            vga::print("    ");
            vga::print(t->name != nullptr ? t->name : "?");
            if (t->state == task::State::Zombie) {
                vga::print(" (exit code ");
                print_int(t->exit_code);
                vga::print(")");
            }
            vga::put_char('\n');
        }
    } else if (token_is(command, token_len, "wait")) {
        vga::print("Waiting for any child of the shell to exit (blocks until one does)...\n");
        task::Pid child_pid = 0;
        int exit_code = 0;
        if (task::wait_for_child(&child_pid, &exit_code)) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
            vga::print("PID ");
            print_uint(child_pid);
            vga::print(" exited with code ");
            print_int(exit_code);
            vga::put_char('\n');
            vga::set_color(vga::Color::White, vga::Color::Black);
        } else {
            vga::print("No child processes to wait for (nothing spawned via 'run' or 'usermode' yet).\n");
        }
    } else if (token_is(command, token_len, "sleep")) {
        if (args[0] == '\0') {
            vga::print("Usage: sleep <ticks> (about 18 ticks per second)\n");
        } else {
            uint64_t ticks = 0;
            for (size_t i = 0; args[i] >= '0' && args[i] <= '9'; ++i) {
                ticks = ticks * 10 + static_cast<uint64_t>(args[i] - '0');
            }
            vga::print("Shell task sleeping - the prompt will pause, then resume on its own...\n");
            task::sleep_current(ticks);
            vga::print("Awake.\n");
        }
    } else {
        vga::print("Unknown command: ");
        vga::print(command);
        vga::put_char('\n');
    }
}

} 

namespace shell {

void init() {
    command_length = 0;
    prompt();
}

void handle_char(char character) {
    if (snake::active()) {
        if (character == 27) {
            snake::stop();
            vga::print("\nGame ended.\n");
            prompt();
        } else {
            snake::handle_key(character);
        }
        return;
    }

    if (character == '\n') {
        vga::put_char('\n');
        execute();
        command_length = 0;
        prompt();
        return;
    }

    if (character == '\b') {
        if (command_length != 0) {
            --command_length;
            vga::put_char('\b');
        }
        return;
    }

    if (character >= ' ' && character <= '~' && command_length + 1 < COMMAND_CAPACITY) {
        command[command_length++] = character;
        vga::put_char(character);
    }
}

void tick() {
    if (snake::active() && !snake::tick()) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::print("Game over.\n");
        prompt();
    }
}

}
