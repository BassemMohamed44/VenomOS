#include "keyboard.hpp"
#include "../include/io.hpp"

namespace
{
const char keymap[128] =
{
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';',39,'`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

const char shifted_keymap[128] =
{
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};
constexpr uint16_t DATA_PORT = 0x60;
constexpr uint16_t STATUS_PORT = 0x64;
constexpr uint8_t STATUS_OUTPUT_FULL = 0x01;
constexpr uint8_t STATUS_INPUT_FULL = 0x02;
constexpr uint8_t CONTROLLER_READ_CONFIG = 0x20;
constexpr uint8_t CONTROLLER_WRITE_CONFIG = 0x60;
constexpr uint8_t CONTROLLER_ENABLE_FIRST_PORT = 0xAE;
constexpr uint8_t CONFIG_FIRST_PORT_INTERRUPT = 0x01;
constexpr uint8_t CONFIG_FIRST_PORT_CLOCK_DISABLED = 0x10;
constexpr uint8_t CONFIG_TRANSLATION = 0x40;
constexpr uint8_t KEYBOARD_ENABLE_SCANNING = 0xF4;
constexpr uint8_t LEFT_SHIFT = 0x2A;
constexpr uint8_t RIGHT_SHIFT = 0x36;
constexpr uint8_t CAPS_LOCK = 0x3A;
constexpr uint8_t EXTENDED_PREFIX = 0xE0;
constexpr uint8_t SCANCODE_UP = 0x48;
constexpr uint8_t SCANCODE_DOWN = 0x50;
constexpr uint8_t SCANCODE_LEFT = 0x4B;
constexpr uint8_t SCANCODE_RIGHT = 0x4D;
constexpr uint8_t QUEUE_SIZE = 64;
volatile char queue[QUEUE_SIZE] = {};
volatile uint8_t queue_head = 0;
volatile uint8_t queue_tail = 0;
bool shift_down = false;
bool caps_lock = false;
bool extended_sequence = false;

bool wait_for_input_empty() {
    for (uint32_t timeout = 0; timeout < 100000; ++timeout) {
        if ((io::inb(STATUS_PORT) & STATUS_INPUT_FULL) == 0) return true;
    }
    return false;
}

bool wait_for_output_full() {
    for (uint32_t timeout = 0; timeout < 100000; ++timeout) {
        if ((io::inb(STATUS_PORT) & STATUS_OUTPUT_FULL) != 0) return true;
    }
    return false;
}

bool write_controller_command(uint8_t command) {
    if (!wait_for_input_empty()) return false;
    io::outb(STATUS_PORT, command);
    return true;
}

bool write_keyboard_data(uint8_t value) {
    if (!wait_for_input_empty()) return false;
    io::outb(DATA_PORT, value);
    return true;
}

void enqueue(char character) {
    const uint8_t next_head = static_cast<uint8_t>((queue_head + 1) % QUEUE_SIZE);
    if (next_head != queue_tail) { queue[queue_head] = character; queue_head = next_head; }
}

char translate(uint8_t scancode) {
    if (scancode >= sizeof(keymap)) return 0;
    char character = keymap[scancode];
    const bool alphabetic = character >= 'a' && character <= 'z';
    if ((alphabetic && (shift_down != caps_lock)) || (!alphabetic && shift_down)) {
        character = shifted_keymap[scancode];
    }
    return character;
}

void process_scancode(uint8_t scancode) {
    if (scancode == EXTENDED_PREFIX) { extended_sequence = true; return; }
    const bool released = (scancode & 0x80) != 0;
    const uint8_t key = static_cast<uint8_t>(scancode & 0x7F);
    if (extended_sequence && !released) {
        if (key == SCANCODE_UP) enqueue(keyboard::KeyUp);
        else if (key == SCANCODE_DOWN) enqueue(keyboard::KeyDown);
        else if (key == SCANCODE_LEFT) enqueue(keyboard::KeyLeft);
        else if (key == SCANCODE_RIGHT) enqueue(keyboard::KeyRight);
        extended_sequence = false;
        return;
    }
    if (key == LEFT_SHIFT || key == RIGHT_SHIFT) { shift_down = !released; extended_sequence = false; return; }
    if (key == CAPS_LOCK && !released) { caps_lock = !caps_lock; extended_sequence = false; return; }
    if (!released && !extended_sequence) { const char character = translate(key); if (character != 0) enqueue(character); }
    extended_sequence = false;
}
}

namespace keyboard
{

void init()
{
    
    if (write_controller_command(CONTROLLER_READ_CONFIG) && wait_for_output_full()) {
        uint8_t config = io::inb(DATA_PORT);
        config |= CONFIG_FIRST_PORT_INTERRUPT | CONFIG_TRANSLATION;
        config &= static_cast<uint8_t>(~CONFIG_FIRST_PORT_CLOCK_DISABLED);

        if (write_controller_command(CONTROLLER_WRITE_CONFIG)) {
            write_keyboard_data(config);
        }
    }

    write_controller_command(CONTROLLER_ENABLE_FIRST_PORT);
    write_keyboard_data(KEYBOARD_ENABLE_SCANNING);
}

bool available()
{
    return queue_head != queue_tail;
}

char read_char()
{
    if (!available()) return 0;
    const char character = queue[queue_tail];
    queue_tail = static_cast<uint8_t>((queue_tail + 1) % QUEUE_SIZE);
    return character;
}

void poll() {
    if ((io::inb(STATUS_PORT) & STATUS_OUTPUT_FULL) == 0) return;
    process_scancode(io::inb(DATA_PORT));
}

void handle_irq() {
    poll();
}

}
