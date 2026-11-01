#pragma once
#include <stddef.h>
#include <stdint.h>

// Rejestr wszystkich parametrów: najpierw wbudowane wartości DEMO (moduł 0, indeksy jak w ParamId),
// dalej katalog z karty SD (/diamel: moduly.csv, parametry.csv, teksty.csv - tylko wartości liczbowe).
// Katalog wczytuje zadanie w tle przy starcie; po catalog_wait() rejestr jest stały do restartu.

typedef uint16_t ParamIdx;
#define PARAM_NONE 0xFFFF
#define MODULE_DEMO 0

enum ParamType : uint8_t { PTYPE_U8, PTYPE_S8, PTYPE_U16, PTYPE_S16, PTYPE_U32, PTYPE_S32, PTYPE_F32 };

struct ParamDef {
    const char *name;
    const char *unit;
    const char *key;   // nazwa w katalogu (np. "motordrehzahl_wert"), "" dla DEMO
    float factor;      // wartość = surowa * factor + offset
    float offset;
    uint32_t mask;     // 0 = bez maski; inaczej (surowa & mask) >> zera na końcu maski
    uint16_t did;
    uint16_t pos;      // pozycja w danych odpowiedzi (po nagłówku 62 xx xx)
    int16_t texts;     // tabela opisów wartości albo -1
    uint8_t addr;      // adres sterownika (0 = DEMO)
    uint8_t module;
    uint8_t type;      // ParamType
    uint8_t decimals;
    uint8_t big_endian;
    uint8_t basic;     // 1 = lista podstawowa (kolumna "poziom"); stary katalog bez kolumny = wszystko podstawowe
};

struct ModuleDef {
    const char *name;
    const char *sgbd;
    uint8_t addr;
    uint16_t first; // pierwszy parametr modułu w rejestrze
    uint16_t count;
    uint16_t dtc_count;
    uint16_t basic_count; // parametry z listy podstawowej
};

struct CatalogStatus {
    bool done;          // wczytywanie zakończone (z sukcesem albo nie)
    bool loaded;        // katalog z karty wczytany
    uint16_t modules;   // moduły z karty
    uint32_t params;    // parametry z karty
    uint32_t skipped;   // pominięte (tekst/bajty - nie liczby)
    uint16_t text_tables;
    uint32_t texts;
    uint16_t dtc_files;
    uint32_t load_ms;
    char error[64];
};

void catalog_begin();                 // po logger_init(): wbudowane parametry + zadanie wczytujące
bool catalog_wait(uint32_t timeout_ms);
int catalog_progress();                // 0-100 % wczytanych plików (pasek na ekranie startowym)
void catalog_status(CatalogStatus &out);

int params_count();
const ParamDef &param(ParamIdx i);
int modules_count();
const ModuleDef &module_def(int m);
int module_by_addr(uint8_t addr);     // -1 = brak

// Opis wartości z tabeli tekstów (np. bieg "3"), NULL gdy brak
const char *param_value_text(ParamIdx i, float value);

// Stały identyfikator parametru do zapisu w ustawieniach (adres + skrót klucza)
uint32_t param_ref(ParamIdx i);
ParamIdx param_by_ref(uint32_t ref);
inline bool ref_is_catalog(uint32_t ref) { return (ref >> 24) != 0; }
// Czytelna nazwa do kopii ustawień: "12:motordrehzahl_wert" (katalog) albo "demo:0" (DEMO) i z powrotem
void param_name_key(ParamIdx i, char *buf, size_t len);
ParamIdx param_by_name_key(const char *text);

// Wyszukiwanie w module (-1 = wszystkie) po nazwie lub kluczu, bez wielkości liter; all = false: tylko lista
// podstawowa. Zapisuje trafienia od `offset` (max `max`), zwraca łączną liczbę trafień.
int params_search(int module, const char *query, int offset, ParamIdx *out, int max, bool all);

// Opisy kodów błędów z /diamel/bledy_XX.csv (czyta kartę - nie z zadania UI)
#define DTC_TEXT_LEN 120
void catalog_dtc_texts(uint8_t addr, const uint32_t *codes, int n, char (*texts)[DTC_TEXT_LEN]);
// Przykładowe kody błędów modułu (tryb DEMO)
int catalog_dtc_samples(uint8_t addr, uint32_t *codes, int max);
