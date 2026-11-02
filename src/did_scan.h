#pragma once
#include <stddef.h>
#include <stdint.h>

// Skan parametrów modułu (tylko odczyt): pytamy o każdy DID modułu z katalogu i zapisujemy wynik
// do /logs/Sxx_....csv (did;wynik;bajty;nazwa). Działa w osobnym zadaniu.
// Tryb zwykły: 22 DID. Tryb zestawu: DID-y po 20 w tymczasowym zestawie F303 (jak odpytywanie DDE);
// gdy sterownik odrzuci zestaw, dzielimy go na pół aż do pojedynczych DID - wynik do /logs/Bxx_....csv.
struct DidScanState {
    bool running;
    uint8_t addr;
    int total, done;
    int ok, refused, silent;
    char file[40];
    char note[64]; // powód przerwania albo ""
};

bool did_scan_start(uint8_t addr, bool block = false); // false = już trwa, komunikacja włączona albo brak karty
void did_scan_stop();
void did_scan_state(DidScanState &out);
