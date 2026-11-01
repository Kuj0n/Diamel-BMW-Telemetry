#pragma once
#include <stdint.h>
#include "telemetry.h"

enum StartScreen : uint8_t { START_MENU, START_LIVE, START_CHART, START_DTC, START_COUNT };

#define LIVE_MAX 30          // maks. liczba wartości na stronie Odczyty
#define CHART_SERIES_MAX 3
#define PARAM_NONE 0xFFFF

// Ustawienia zapisywane w pamięci NVS (przetrwają wyłączenie zasilania).
// Nowe pola dopisujemy WYŁĄCZNIE na końcu - starsze zapisy wczytają się, a nowe pola dostaną wartości domyślne.
struct Settings {
    uint8_t version;
    uint8_t brightness;
    uint8_t start_screen;
    uint8_t live_count;
    uint16_t live_ids[LIVE_MAX];             // wybrane wartości strony Odczyty, w kolejności
    uint16_t chart_series[CHART_SERIES_MAX]; // ParamId lub PARAM_NONE
    uint16_t poll_interval_ms;  // odstęp między zapytaniami o wartości
    uint16_t poll_timeout_ms;   // czas oczekiwania na odpowiedź (wartości)
    uint16_t dtc_timeout_ms;    // czas oczekiwania na odpowiedź (błędy)
    uint16_t stop_no_reply_s;   // zatrzymaj odpytywanie, gdy sterownik nie odpowiada (zapłon wyłączony)
    uint16_t stop_rpm_zero_min; // zatrzymaj odpytywanie po N min z obrotami 0 (0 = wyłączone)
    uint16_t log_interval_ms;   // odstęp między wierszami logu CSV
};

extern Settings g_settings;

void settings_load();
void settings_save();
// Zapis z opóźnieniem - kilka szybkich zmian (np. przyciski +/-) daje jeden zapis do flash
void settings_mark_dirty();
// Wywoływane cyklicznie; zapisuje oczekujące zmiany
void settings_tick();
