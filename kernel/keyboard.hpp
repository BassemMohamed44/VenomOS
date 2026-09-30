#pragma once

namespace keyboard
{
    constexpr char KeyUp = 0x11;
    constexpr char KeyDown = 0x12;
    constexpr char KeyLeft = 0x13;
    constexpr char KeyRight = 0x14;

    void init();
    bool available();
    char read_char();
    void poll();
    void handle_irq();
}
