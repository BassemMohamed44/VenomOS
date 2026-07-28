#include "snake.hpp"

#include "keyboard.hpp"
#include "vga.hpp"

namespace {

constexpr unsigned int BOARD_WIDTH = 50;
constexpr unsigned int BOARD_HEIGHT = 17;
constexpr unsigned int MAX_SEGMENTS = 128;
constexpr unsigned int MOVE_INTERVAL_TICKS = 3;

struct Point {
    int x;
    int y;
};

enum class Direction { Up, Down, Left, Right };

Point body[MAX_SEGMENTS] = {};
Point food = {};
unsigned int length = 0;
unsigned int tick_count = 0;
uint32_t random_state = 0x56E0BEEF;
Direction direction = Direction::Right;
Direction requested_direction = Direction::Right;
bool running = false;

bool occupies(Point point, unsigned int count) {
    for (unsigned int index = 0; index < count; ++index) {
        if (body[index].x == point.x && body[index].y == point.y) return true;
    }
    return false;
}

void spawn_food() {
    do {
        random_state = random_state * 1664525U + 1013904223U;
        food.x = 1 + static_cast<int>(random_state % BOARD_WIDTH);
        random_state = random_state * 1664525U + 1013904223U;
        food.y = 1 + static_cast<int>(random_state % BOARD_HEIGHT);
    } while (occupies(food, length));
}

void render() {
    vga::clear(vga::Color::LightGrey, vga::Color::Black);
    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    vga::print("VenomOS Snake  |  Arrow keys: move  |  Esc: exit\n");

    for (unsigned int y = 0; y <= BOARD_HEIGHT + 1; ++y) {
        for (unsigned int x = 0; x <= BOARD_WIDTH + 1; ++x) {
            char character = ' ';
            if (x == 0 || x == BOARD_WIDTH + 1 || y == 0 || y == BOARD_HEIGHT + 1) {
                character = '#';
            } else if (food.x == static_cast<int>(x) && food.y == static_cast<int>(y)) {
                character = '*';
            } else {
                for (unsigned int index = 0; index < length; ++index) {
                    if (body[index].x == static_cast<int>(x) && body[index].y == static_cast<int>(y)) {
                        character = index == 0 ? '@' : 'O';
                        break;
                    }
                }
            }
            vga::put_char(character);
        }
        vga::put_char('\n');
    }
}

bool is_opposite(Direction first, Direction second) {
    return (first == Direction::Up && second == Direction::Down) ||
           (first == Direction::Down && second == Direction::Up) ||
           (first == Direction::Left && second == Direction::Right) ||
           (first == Direction::Right && second == Direction::Left);
}

} // namespace

namespace snake {

void start() {
    length = 3;
    body[0] = { 25, 9 };
    body[1] = { 24, 9 };
    body[2] = { 23, 9 };
    direction = Direction::Right;
    requested_direction = direction;
    tick_count = 0;
    running = true;
    spawn_food();
    render();
}

void stop() {
    running = false;
}

bool active() {
    return running;
}

void handle_key(char key) {
    Direction candidate = requested_direction;
    if (key == keyboard::KeyUp) candidate = Direction::Up;
    else if (key == keyboard::KeyDown) candidate = Direction::Down;
    else if (key == keyboard::KeyLeft) candidate = Direction::Left;
    else if (key == keyboard::KeyRight) candidate = Direction::Right;
    else return;

    if (!is_opposite(direction, candidate)) requested_direction = candidate;
}

bool tick() {
    if (!running || ++tick_count < MOVE_INTERVAL_TICKS) return running;
    tick_count = 0;
    direction = requested_direction;

    Point next = body[0];
    if (direction == Direction::Up) --next.y;
    else if (direction == Direction::Down) ++next.y;
    else if (direction == Direction::Left) --next.x;
    else ++next.x;

    const bool ate_food = next.x == food.x && next.y == food.y;
    const unsigned int collision_count = ate_food ? length : length - 1;
    if (next.x < 1 || next.x > static_cast<int>(BOARD_WIDTH) ||
        next.y < 1 || next.y > static_cast<int>(BOARD_HEIGHT) || occupies(next, collision_count)) {
        running = false;
        return false;
    }

    const unsigned int new_length = ate_food && length < MAX_SEGMENTS ? length + 1 : length;
    for (unsigned int index = new_length - 1; index > 0; --index) body[index] = body[index - 1];
    body[0] = next;
    length = new_length;
    if (ate_food) spawn_food();
    render();
    return true;
}

} // namespace snake
