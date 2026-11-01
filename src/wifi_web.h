#pragma once
#include <stdint.h>

// Własna sieć Wi-Fi (punkt dostępowy) i serwer WWW na telefon:
// podgląd wartości na żywo, logi z pobieraniem/usuwaniem, wybór odczytów i serii wykresu,
// aktualizacja oprogramowania (OTA). Włączane ręcznie ze strony Wi-Fi na ekranie.
// Serwer działa w zadaniu AsyncTCP na rdzeniu 0 - nie dotyka LVGL; zmiany dla interfejsu
// przekazuje przez wifi_apply_web_settings() / wifi_take_message() wołane z zadania UI.
bool wifi_start(); // blokuje ok. 0.5 s; false = nie udało się uruchomić punktu dostępowego
void wifi_stop();
bool wifi_active();
uint8_t wifi_clients();
const char *wifi_ssid(); // "Diamel-XXXX" z końcówki adresu MAC, znane także przed włączeniem
const char *wifi_ip();

// Aktualizacja OTA: -1 = brak, 0-100 = postęp
int wifi_ota_progress();

// Z zadania UI: przenosi do g_settings zmiany zapisane w przeglądarce. true = coś się zmieniło.
bool wifi_apply_web_settings();
// Z zadania UI: restart po udanej aktualizacji (najpierw zamyka log), przerwanie zawieszonej aktualizacji
void wifi_tick();
// Z zadania UI: komunikat do pokazania raz (np. wynik aktualizacji), NULL = brak
const char *wifi_take_message();
