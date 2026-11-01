#pragma once
#include <stdint.h>

// Karta microSD i zapis logów CSV (zadanie w tle na rdzeniu 0).
// Kolumny: czas + wartości wybrane na stronie Odczyty i na wykresie.
#define LOG_DIR "/logs"

void logger_init();      // montuje kartę i uruchamia zadanie zapisu (po inicjalizacji ekranu - wspólne piny)
bool logger_sd_ok();
bool logger_start();     // false = brak karty
void logger_stop();
bool logger_active();
uint32_t logger_elapsed_ms();
const char *logger_current_file(); // nazwa pliku bez katalogu, "" gdy nie nagrywa
bool logger_take_error();          // true raz po błędzie zapisu (np. wyjęta karta)
