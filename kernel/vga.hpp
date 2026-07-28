// ============================================================
// vga.hpp
//
// Driver for the standard VGA text mode (80x25, 16 colors),
// which the BIOS leaves the machine in by default - we don't
// have to program any VGA registers to get this mode, only to
// enhance it (colors, hardware cursor).
// ============================================================
#pragma once

#include "stddef.hpp"
#include "stdint.hpp"

namespace vga {

// The 16 standard VGA text-mode colors. Values match the VGA
// attribute byte's color indices exactly (0-15).
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

// Must be called once before any other vga:: function - resets
// cursor position and applies the default color scheme.
void init();

// Clears the entire screen to the given colors and resets the
// cursor to the top-left corner.
void clear(Color fg = Color::LightGrey, Color bg = Color::Black);

// Changes the color used by subsequent put_char()/print() calls.
// Does not affect characters already on screen.
void set_color(Color fg, Color bg);

// Writes a single character at the current cursor position and
// advances the cursor, handling '\n' and end-of-line wraparound
// and scrolling the screen up when the last line is exceeded.
void put_char(char c);

// Writes a null-terminated string using the current color.
void print(const char *str);

// Writes a null-terminated string in a specific color, then
// restores whatever color was active before the call.
void print_colored(const char *str, Color fg, Color bg = Color::Black);

} // namespace vga
