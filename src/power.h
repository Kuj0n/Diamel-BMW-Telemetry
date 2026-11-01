#pragma once
#include <stdint.h>

// Automatyczne uśpienie: gdy wybrany parametr jest mniejszy/większy od progu (albo brak danych)
// nieprzerwanie przez ustawiony czas - koniec nagrywania, zgaszony ekran. Dotyk budzi.
// Odpytywanie CAN (gdy powstanie) sprawdza power_sleeping() i wstrzymuje zapytania.
void power_tick();       // z zadania UI, co 100 ms
bool power_sleeping();
int power_seconds_left(); // do uśpienia; -1 = warunek niespełniony albo funkcja wyłączona
