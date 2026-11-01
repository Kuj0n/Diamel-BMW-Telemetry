#pragma once
#include <stddef.h>
#include <stdint.h>
#include "catalog.h"

// Odczyt kodów błędów - tylko na żądanie (przycisk), nigdy cyklicznie. Tylko odczyt: kasowania nie ma.
// Auto: UDS 0x19 02 (kody wg maski statusu) dla każdego modułu. Opisy z /diamel/bledy_XX.csv.
// W trybie DEMO - przykładowe kody z katalogu (symulacja).

enum DtcState : uint8_t { DTC_IDLE, DTC_READING, DTC_DONE, DTC_FAILED };

#define DTC_MAX 200

struct DtcEntry {
    uint8_t module;
    uint32_t code;
    uint8_t status;  // bajt statusu UDS (bit 0 = błąd aktywny teraz)
    char text[DTC_TEXT_LEN];
};

void dtc_read(int module); // -1 = wszystkie moduły z plikami błędów
DtcState dtc_state();
void dtc_status_text(char *buf, size_t len);
int dtc_count();                 // wyniki ważne w stanie DTC_DONE
const DtcEntry &dtc_entry(int i);
bool dtc_simulated();            // wyniki z trybu DEMO
// Zapis wyników do pliku CSV w katalogu logów (pobieranie: strona Pliki / WWW). false = brak karty/wyników
bool dtc_export(char *name, size_t len);
