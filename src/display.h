#pragma once
#include <stdint.h>

// Inicjalizacja ekranu RGB, LVGL (bufory w PSRAM) i dotyku GT911
void display_init();
// Jasność podświetlenia 0-255
void display_set_brightness(uint8_t level);
uint8_t display_get_brightness();
bool display_touch_ok();
// Klatki na sekundę od poprzedniego wywołania (wołać co ok. 0.5-1 s)
uint16_t display_fps();
// Uśpienie ekranu: dotyk nie klika, tylko zgłasza chęć wybudzenia (display_take_wake)
void display_sleep(bool on);
bool display_take_wake();
// Obraz ekranu (RGB565, 480x480, w PSRAM) - do zrzutów ekranu przez WWW
const uint16_t *display_framebuffer();
