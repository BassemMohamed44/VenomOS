#pragma once

#include "stddef.hpp"
#include "stdint.hpp"

namespace vga {

enum class Color : uint8_t {
  Black = 0,
  Blue = 1,
  Green = 2,
  Cyan = 3,
  Red = 4,
  Magenta = 5,
  Brown = 6,
  LightGrey = 7,
  DarkGrey = 8,
  LightBlue = 9,
  LightGreen = 10,
  LightCyan = 11,
  LightRed = 12,
  LightMagenta = 13,
  Yellow = 14,
  White = 15,
};

void init();
void clear(Color fg = Color::LightGrey, Color bg = Color::Black);
void set_color(Color fg, Color bg);
void put_char(char c);
void print(const char *str);
void print_colored(const char *str, Color fg, Color bg = Color::Black);
void put_char_at(size_t row, size_t col, char c, Color fg, Color bg = Color::Black);

}
