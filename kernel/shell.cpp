#include "shell.hpp"

#include "snake.hpp"
#include "vga.hpp"

namespace {

constexpr unsigned int COMMAND_CAPACITY = 80;
char command[COMMAND_CAPACITY] = {};
unsigned int command_length = 0;

bool equals(const char* left, const char* right) {
    for (unsigned int index = 0;; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
    }
}

void prompt() {
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::print("VenomOS> ");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

void execute() {
    command[command_length] = '\0';

    if (command_length == 0) {
        return;
    }
    if (equals(command, "help")) {
        vga::print("Commands: help, clear, version, whoami, snake\n");
    } else if (equals(command, "clear")) {
        vga::clear(vga::Color::LightGrey, vga::Color::Black);
    } else if (equals(command, "version")) {
        vga::print("VenomOS version 0.2.0 (x86_64)\n");
    } else if (equals(command, "whoami")) {
        vga::print("desktop-venomos\\ghost user\n");
    } else if (equals(command, "snake")) {
        snake::start();
    } else {
        vga::print("Unknown command: ");
        vga::print(command);
        vga::put_char('\n');
    }
}

} // namespace

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

} // namespace shell
