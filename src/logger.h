#pragma once
#include <stddef.h>
#include <stdint.h>

// Karta microSD i zapis logów CSV (zadanie w tle na rdzeniu 0).
// Kolumny: czas + wartości wybrane na stronie Odczyty i na wykresie.
#define LOG_DIR "/logs"
#define CATALOG_DIR "/diamel" // katalog parametrów i kodów błędów (pliki CSV wgrywane przez WWW)

void logger_init();      // montuje kartę i uruchamia zadanie zapisu (po inicjalizacji ekranu - wspólne piny)
bool logger_sd_ok();
bool logger_start();     // false = brak karty
void logger_stop();
bool logger_active();
uint32_t logger_elapsed_ms();
const char *logger_current_file(); // nazwa pliku bez katalogu, "" gdy nie nagrywa
bool logger_take_error();          // true raz po błędzie zapisu (np. wyjęta karta)
// Nazwa pliku z katalogu logów bez ścieżki ("LOG_0001.csv") - chroni przed "../" z serwera WWW
bool logger_valid_name(const char *name);
// Usuwa log; false = zła nazwa, plik w trakcie zapisu albo błąd karty
bool logger_delete(const char *name);

// Formatowanie karty (FAT32, cała karta) w zadaniu w tle - trwa kilkanaście sekund.
// Uruchamiane z ekranu albo ze strony WWW.
// false = nie można zacząć (nagrywanie trwa albo formatowanie już działa).
bool logger_format_start();
bool logger_formatting();
bool logger_format_ok(); // wynik ostatniego formatowania
const char *logger_format_error(); // powód błędu albo ""
// Nowa nazwa pliku w katalogu logów: PRE_RRRRMMDD_GGMMSS.csv (zegar ustawiony) albo PRE_0001.csv
void logger_new_name(const char *prefix, char *name, size_t len);
