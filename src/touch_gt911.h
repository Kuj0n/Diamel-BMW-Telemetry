#pragma once
#include <stdint.h>

// Minimalny sterownik dotyku GT911 (odpytywanie przez I2C, bez linii INT)
bool gt911_begin(int sda, int scl);
// Zwraca true, gdy ekran jest dotknięty; x/y to surowe współrzędne kontrolera
bool gt911_read(uint16_t &x, uint16_t &y);
uint8_t gt911_address();
uint16_t gt911_max_x();
uint16_t gt911_max_y();
