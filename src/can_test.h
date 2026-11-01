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
