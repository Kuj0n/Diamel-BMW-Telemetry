#pragma once
#include <stdint.h>
#include "catalog.h"
#include "telemetry.h"

enum StartScreen : uint8_t { START_MENU, START_LIVE, START_CHART, START_DTC, START_COUNT };
// Excel PL: separator ";" i przecinek dziesiętny; standard: "," i kropka
enum CsvFormat : uint8_t { CSV_EXCEL_PL, CSV_STANDARD, CSV_COUNT };
// Skąd pochodzą wartości: symulacja przy biurku albo auto
// (transceiver CAN w gnieździe OBD albo adapter ELM327 przez Bluetooth Low Energy)
enum DataSource : uint8_t { SRC_DEMO, SRC_CAN_DIRECT, SRC_BLE, SRC_COUNT };

#define LIVE_MAX 30          // maks. liczba wartości na stronie Odczyty
#define CHART_SERIES_MAX 3
#define WIFI_PASS_LEN 10

// Ustawienia zapisywane w pamięci NVS (przetrwają wyłączenie zasilania).
// Nowe pola dopisujemy WYŁĄCZNIE na końcu - starsze zapisy wczytają się, a nowe pola dostaną wartości domyślne.
struct Settings {
    uint8_t version;
    uint8_t brightness;
    uint8_t start_screen;
    uint8_t live_count;
    // Indeksy w rejestrze parametrów (catalog.h) - ważne tylko do restartu; trwale zapisywane są
    // identyfikatory live_refs/chart_refs (na końcu struktury), przeliczane po wczytaniu katalogu
    uint16_t live_ids[LIVE_MAX];             // wybrane wartości strony Odczyty, w kolejności
    uint16_t chart_series[CHART_SERIES_MAX]; // ParamIdx lub PARAM_NONE
    uint16_t poll_interval_ms;  // odstęp między zapytaniami o wartości
    uint16_t poll_timeout_ms;   // czas oczekiwania na odpowiedź (wartości)
    uint16_t dtc_timeout_ms;    // czas oczekiwania na odpowiedź (błędy)
    uint16_t stop_no_reply_s;   // zatrzymaj odpytywanie, gdy sterownik nie odpowiada (zapłon wyłączony)
    uint16_t stop_rpm_zero_min; // zatrzymaj odpytywanie po N min z obrotami 0 (0 = wyłączone)
    uint16_t log_interval_ms;   // odstęp między wierszami logu CSV
    uint8_t csv_format;         // CsvFormat
    char wifi_pass[WIFI_PASS_LEN + 1]; // hasło sieci Wi-Fi, losowane przy pierwszym starcie
    uint8_t data_source;        // DataSource
    uint8_t show_fps;           // licznik klatek w rogu ekranu
    uint8_t show_cpu;           // obciążenie procesora (grafika) w rogu ekranu
    char ble_addr[18];          // wybrany adapter BLE "aa:bb:cc:dd:ee:ff", "" = brak
    char ble_name[24];
    uint8_t ble_addr_type;      // typ adresu BLE (publiczny / losowy) potrzebny do połączenia
    uint32_t live_refs[LIVE_MAX];          // param_ref() wybranych wartości
    uint32_t chart_refs[CHART_SERIES_MAX]; // param_ref() albo PARAM_NONE
    uint8_t live_ref_count;
    uint8_t refs_magic;                    // REFS_MAGIC = pola *_refs wypełnione
    // Automatyczne uśpienie: parametr (mniejszy/większy niż próg) przez sleep_delay_s sekund
    uint16_t sleep_param;       // indeks w rejestrze (jak live_ids), trwale: sleep_ref
    uint32_t sleep_ref;
    float sleep_threshold;
    uint16_t sleep_delay_s;
    uint8_t sleep_enabled;
    uint8_t sleep_above;        // 0 = mniejszy niż próg, 1 = większy niż próg
    uint8_t sleep_on_missing;   // brak wartości (np. zapłon wyłączony) = warunek spełniony
    uint8_t comm_autostart;     // rozpocznij komunikację z autem zaraz po uruchomieniu
};

extern Settings g_settings;

void settings_load();
// Po wczytaniu katalogu: identyfikatory zapisanych parametrów -> indeksy w rejestrze
void settings_resolve_params();
void settings_save();
// Zapis z opóźnieniem - kilka szybkich zmian (np. przyciski +/-) daje jeden zapis do flash
void settings_mark_dirty();
// Wywoływane cyklicznie; zapisuje oczekujące zmiany
void settings_tick();
// Zapis od razu, jeśli są niezapisane zmiany (np. przed restartem)
void settings_flush();
// Losuje nowe hasło Wi-Fi i zapisuje ustawienia
void settings_new_wifi_password();
// Krótka nazwa źródła danych do paska statusu / strony WWW
const char *data_source_name(uint8_t source);
