#pragma once
#include <stddef.h>
#include <stdint.h>

// Adapter OBD z interfejsem ELM327 przez Bluetooth Low Energy (np. vLinker MC+, OBDLink CX).
// Adaptery na klasyczny Bluetooth (SPP) nie zadziałają - ESP32-S3 ma tylko BLE.
// Wszystkie operacje radiowe wykonuje zadanie w tle na rdzeniu 0; funkcje dla UI tylko zlecają
// i odczytują stan. Bluetooth startuje dopiero przy pierwszym użyciu (zajmuje pamięć RAM).

enum BleState : uint8_t { BLE_OFF, BLE_SCANNING, BLE_CONNECTING, BLE_CONNECTED, BLE_TESTING };

#define BLE_FOUND_MAX 20

struct BleFound {
    char name[24];
    char addr[18];
    uint8_t addr_type;
    int8_t rssi;
};

// Zlecenia dla zadania w tle (UI)
void ble_scan();                              // ok. 5 s szukania urządzeń z nazwą
void ble_connect(const char *addr, uint8_t addr_type);
void ble_disconnect();
void ble_test();                              // ATZ / ATI / AT@1 / ATRV - opis adaptera i napięcie

// Stan dla UI (bezpieczne między zadaniami)
BleState ble_state();
int ble_found(BleFound *out, int max);        // kopia listy znalezionych, najsilniejszy sygnał pierwszy
void ble_status_text(char *buf, size_t len);  // ostatni komunikat, np. "Polaczono" / "Blad: ..."
void ble_test_text(char *buf, size_t len);    // wynik testu adaptera

// Komenda ELM327 - tylko z zadań innych niż UI (blokuje do odpowiedzi ">" albo timeoutu)
bool ble_elm_command(const char *cmd, char *resp, size_t len, uint32_t timeout_ms);
