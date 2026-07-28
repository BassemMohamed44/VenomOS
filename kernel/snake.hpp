#pragma once

namespace snake {

void start();
void stop();
bool active();
void handle_key(char key);
bool tick();

} // namespace snake
