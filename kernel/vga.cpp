#include "vga.hpp"
#include "../include/io.hpp"

namespace vga {

namespace {

constexpr size_t VGA_WIDTH  = 80;
constexpr size_t VGA_HEIGHT = 25;

uint16_t* const vga_buffer = reinterpret_cast<uint16_t*>(0xB8000);

size_t   cursor_row = 0;
size_t   cursor_col = 0;
uint8_t  current_color = 0;

constexpr uint8_t make_color(Color fg, Color bg) {
    return static_cast<uint8_t>(fg) | (static_cast<uint8_t>(bg) << 4);
}

constexpr uint16_t make_entry(char c, uint8_t color) {
    return static_cast<uint16_t>(static_cast<uint8_t>(c)) |
           (static_cast<uint16_t>(color) << 8);
}


void update_hardware_cursor() {
    uint16_t position = static_cast<uint16_t>(cursor_row * VGA_WIDTH + cursor_col);

    io::outb(0x3D4, 0x0F);                             
    io::outb(0x3D5, static_cast<uint8_t>(position & 0xFF));
    io::outb(0x3D4, 0x0E);                        
    io::outb(0x3D5, static_cast<uint8_t>((position >> 8) & 0xFF));
}


void scroll() {
    for (size_t row = 1; row < VGA_HEIGHT; ++row) {
        for (size_t col = 0; col < VGA_WIDTH; ++col) {
            vga_buffer[(row - 1) * VGA_WIDTH + col] = vga_buffer[row * VGA_WIDTH + col];
        }
    }
    for (size_t col = 0; col < VGA_WIDTH; ++col) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = make_entry(' ', current_color);
    }
    cursor_row = VGA_HEIGHT - 1;
    cursor_col = 0;
}

} 

void init() {
    current_color = make_color(Color::LightGrey, Color::Black);
    cursor_row = 0;
    cursor_col = 0;
    update_hardware_cursor();
}

void clear(Color fg, Color bg) {
    current_color = make_color(fg, bg);
    for (size_t row = 0; row < VGA_HEIGHT; ++row) {
        for (size_t col = 0; col < VGA_WIDTH; ++col) {
            vga_buffer[row * VGA_WIDTH + col] = make_entry(' ', current_color);
        }
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hardware_cursor();
}

void set_color(Color fg, Color bg) {
    current_color = make_color(fg, bg);
}

void put_char(char c) {
    if (c == '\n') {
        cursor_col = 0;
        ++cursor_row;
    } else if (c == '\b') {
        if (cursor_col != 0) {
            --cursor_col;
        } else if (cursor_row != 0) {
            --cursor_row;
            cursor_col = VGA_WIDTH - 1;
        } else {
            update_hardware_cursor();
            return;
        }
        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = make_entry(' ', current_color);
    } else {
        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = make_entry(c, current_color);
        ++cursor_col;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            ++cursor_row;
        }
    }

    if (cursor_row >= VGA_HEIGHT) {
        scroll();
    }

    update_hardware_cursor();
}

void print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; ++i) {
        put_char(str[i]);
    }
}

void print_colored(const char* str, Color fg, Color bg) {
    uint8_t saved_color = current_color;
    set_color(fg, bg);
    print(str);
    current_color = saved_color;
}

void put_char_at(size_t row, size_t col, char c, Color fg, Color bg) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    vga_buffer[row * VGA_WIDTH + col] = make_entry(c, make_color(fg, bg));
}

}
