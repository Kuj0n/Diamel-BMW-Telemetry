#pragma once
#include <stdint.h>

// Autotest toru CAN bez auta: kontroler TWAI w trybie bez potwierdzeń (NO_ACK) wysyła ramki
// i odbiera je z powrotem przez transceiver. Sprawdza pin TX, zasilanie i działanie
// transceivera oraz pin RX. Test NADAJE na magistralę - tylko z wtyczką odłączoną od auta.
struct CanTestResult {
    bool driver_ok;     // sterownik TWAI uruchomiony
    int sent;           // ramki wysłane
    int received;       // ramki odebrane z powrotem
    uint32_t tx_errors; // licznik błędów nadawania po teście
    uint32_t rx_errors; // licznik błędów odbioru po teście
    bool bus_off;       // kontroler wyłączył się z magistrali (poważne błędy)
};

bool can_self_test(CanTestResult &r);

// Test w aucie (tylko odczyt): 1) nasłuch bez nadawania i bez potwierdzeń (LISTEN_ONLY),
// 2) jedno zapytanie odczytu do DDE (0x12: 22 F1 90 - numer VIN) i zapis wszystkich ramek odpowiedzi.
#define CAR_TEST_IDS 8
#define CAR_TEST_FRAMES 6
struct CanCarResult {
    bool driver_ok;
    int listen_frames;                // ramki usłyszane w nasłuchu
    int listen_ids;                   // liczba różnych identyfikatorów
    uint32_t ids[CAR_TEST_IDS];       // pierwsze identyfikatory
    uint32_t listen_errors;           // błędy odbioru w nasłuchu (zła prędkość, zamienione CANH/CANL, brak masy)
    int normal_frames;                // ramki w zwykłym trybie (my potwierdzamy) - bez nadawania zapytań
    int attempts;                     // próby wysłania zapytania
    uint32_t arb_lost;                // przegrane arbitraże (magistrala zajęta ramką o wyższym priorytecie)
    uint32_t bus_errors;              // błędy magistrali w zwykłym trybie
    bool tx_acked;                    // ktoś potwierdził naszą ramkę (ACK)
    int reply_count;                  // wszystkie ramki po zapytaniu
    int frame_count;                  // zapisane ramki (do CAR_TEST_FRAMES)
    uint32_t frame_id[CAR_TEST_FRAMES];
    uint8_t frame_dlc[CAR_TEST_FRAMES];
    uint8_t frame_data[CAR_TEST_FRAMES][8];
    bool dde_reply;                   // odpowiedź z 0x612 adresowana do testera
    uint32_t tx_errors, rx_errors;
    bool bus_off;
    bool err_valid;                   // rodzaj ostatniego błędu nadawania (rejestr kontrolera)
    uint8_t err_type;                 // 0 bit, 1 forma, 2 bit stuffing, 3 inny (np. brak ACK)
    uint8_t err_seg;                  // miejsce w ramce: 0x19 = pole ACK
    bool err_tx;                      // błąd przy nadawaniu (a nie odbiorze)
};

bool can_car_test(CanCarResult &r);

// Standardowe OBD-II (ISO 15765-4): zapytanie ogólne 0x7DF, tryb 01 (bieżące dane, tylko odczyt),
// odpowiedź silnika 0x7E8. Kolejne PID z listy; odpowiedź = surowe bajty danych.
#define OBD_PIDS 6
struct ObdResult {
    uint8_t pid[OBD_PIDS];
    bool ok[OBD_PIDS];
    uint32_t from[OBD_PIDS]; // identyfikator odpowiedzi (0x7E8 = silnik)
    uint8_t data[OBD_PIDS][4];
    uint8_t len[OBD_PIDS];
};
bool can_obd2_probe(ObdResult &r);
