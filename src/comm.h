#pragma once
#include <stddef.h>
#include <stdint.h>

// Komunikacja z autem - TYLKO ODCZYT.
// Transport: transceiver CAN w gnieździe OBD (D-CAN 500 kbit/s), ISO-TP z adresowaniem rozszerzonym BMW:
// zapytanie CAN ID 0x6F1 [adres sterownika, PCI, dane], odpowiedź 0x600+adres [0xF1, PCI, dane].
// Blokada usług: przepuszczane są wyłącznie 0x22 (odczyt DID), 0x19 (odczyt błędów), 0x3E (podtrzymanie).
// Wszystko inne (zapis, kasowanie błędów, procedury, programowanie) jest odrzucane przed wysłaniem.

enum CommState : uint8_t { COMM_STOPPED, COMM_RUNNING, COMM_ERROR };

void comm_init();         // po wczytaniu katalogu; autostart wg ustawień
void comm_start();        // odpytywanie wybranych parametrów
void comm_stop();
bool comm_running();      // włączone przez użytkownika (stan może być też COMM_ERROR)
CommState comm_state();
void comm_status_text(char *buf, size_t len);
bool comm_value(uint16_t idx, float &out); // ostatnia świeża wartość odczytana z auta

enum UdsResult : uint8_t { UDS_OK, UDS_NO_BUS, UDS_NO_REPLY, UDS_NEGATIVE, UDS_BLOCKED, UDS_NO_TRANSPORT };
const char *uds_result_text(UdsResult r);
// Jedno zapytanie diagnostyczne (blokujące, nie z zadania UI). resp = odpowiedź bez nagłówka ISO-TP.
UdsResult uds_request(uint8_t ecu, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, size_t *resp_len,
                      uint32_t timeout_ms);
// Zwalnia kontroler CAN (np. przed testem toru CAN na stronie Info); false = komunikacja włączona
bool uds_release();
