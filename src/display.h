#pragma once
#include <stdint.h>

// Inicjalizacja ekranu RGB, LVGL (bufory w PSRAM) i dotyku GT911
void display_init();
// Jasność podświetlenia 0-255
void display_set_brightness(uint8_t level);
uint8_t display_get_brightness();
bool display_touch_ok();
