#pragma once
#include <stddef.h>
#include <stdint.h>

// Komunikacja z autem - odczyt. Jedyny zapis: kasowanie kodów błędów jednego sterownika (uds_clear_dtc),
// wyłącznie na wyraźne polecenie z ekranu Błędy.
// Transport: transceiver CAN w gnieździe OBD (D-CAN 500 kbit/s), ISO-TP z adresowaniem rozszerzonym BMW:
// zapytanie CAN ID 0x6F1 [adres sterownika, PCI, dane], odpowiedź 0x600+adres [0xF1, PCI, dane].
// Blokada usług: przepuszczane są wyłącznie 0x22 (odczyt DID), 0x19 (odczyt błędów), 0x3E (podtrzymanie)
// oraz zmiana sesji 0x10 tylko na zwykłą (01) i rozszerzoną (03) - nigdy programowania (02).
// Wszystko inne (zapis, kasowanie błędów, procedury, programowanie) jest odrzucane przed wysłaniem.

enum CommState : uint8_t { COMM_STOPPED, COMM_RUNNING, COMM_ERROR };

void comm_init();         // po wczytaniu katalogu; autostart wg ustawień
void comm_start();        // odpytywanie wybranych parametrów
void comm_stop();
bool comm_running();      // włączone przez użytkownika (stan może być też COMM_ERROR)
CommState comm_state();
void comm_status_text(char *buf, size_t len);
bool comm_value(uint16_t idx, float &out); // ostatnia świeża wartość odczytana z auta
// Co jest na ekranie: odpytujemy tylko to (plus nagrywany log, parametr usypiania i podgląd WWW)
#define VIEW_NONE -1
#define VIEW_CHART 3 // 0..2 = strona Odczytów
#define VIEW_LIST 4  // stała lista (np. kafelek DPF) - comm_set_view_list()
void comm_set_view(int view);
void comm_set_view_list(const uint16_t *ids, int n);
// Pełne cykle odczytu na sekundę (x10) - wskaźnik szybkości; 0 = brak odczytów
uint16_t comm_rate_x10();
// Podgląd na stronie WWW: te parametry są odpytywane przez kilka sekund od ostatniego zapytania
void comm_web_watch(const uint16_t *ids, int n);

enum UdsResult : uint8_t { UDS_OK, UDS_NO_BUS, UDS_NO_REPLY, UDS_NEGATIVE, UDS_BLOCKED, UDS_NO_TRANSPORT };
const char *uds_result_text(UdsResult r);
// Jedno zapytanie diagnostyczne (blokujące, nie z zadania UI). resp = odpowiedź bez nagłówka ISO-TP.
UdsResult uds_request(uint8_t ecu, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, size_t *resp_len,
                      uint32_t timeout_ms);
// Zwalnia kontroler CAN (np. przed testem toru CAN na stronie Info); false = komunikacja włączona
bool uds_release();
// Kasowanie wszystkich kodów błędów jednego sterownika: UDS 14 FF FF FF (gdy sterownik wymaga - w sesji 10 03).
// Jedyna usługa zapisu w urządzeniu; nie da się nią wysłać nic innego.
UdsResult uds_clear_dtc(uint8_t ecu, uint8_t *nrc);
// Próba zestawu dynamicznego (test parametrów): definiuje F303 z podanych DID (pozycja 1, rozmiar),
// czyta go i czyści. resp = odpowiedź na 22 F303 (albo odmowa definicji).
UdsResult uds_block_probe(uint8_t ecu, const uint16_t *dids, const uint8_t *sizes, int n, uint8_t *resp, size_t cap,
                          size_t *len);
// Standardowe OBD-II, tryb 01 (bieżące dane, tylko odczyt): zapytanie ogólne 0x7DF, odpowiedź 0x7E8-0x7EF.
// Parametry z katalogu o adresie OBD_ADDR: did = numer PID, pozycja liczona od bajtu A.
#define OBD_ADDR 0xEF
// Diagnostyka ostatniego zapytania: ile ramek przyszło w czasie oczekiwania i ostatnia z nich
struct UdsRxDebug {
    uint16_t frames;
    uint32_t last_id;
    uint8_t last[8];
};
void uds_last_rx(UdsRxDebug &out);
// resp = "41 PID A B ..."
UdsResult obd_request(uint8_t pid, uint8_t *resp, size_t cap, size_t *resp_len, uint32_t timeout_ms);
