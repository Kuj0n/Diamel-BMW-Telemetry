#include "can_test.h"
#include <Arduino.h>
#include "driver/twai.h"
#include "config.h"

#define TEST_FRAMES 10
#define TEST_ID 0x7A5 // identyfikator spoza zakresu diagnostyki BMW (0x6xx)

bool can_self_test(CanTestResult &r) {
    memset(&r, 0, sizeof(r));

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NO_ACK);
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }
    r.driver_ok = true;

    for (int i = 0; i < TEST_FRAMES; i++) {
        twai_message_t tx = {};
        tx.identifier = TEST_ID;
        tx.self = 1; // odbiór własnej ramki
        tx.data_length_code = 8;
        for (int b = 0; b < 8; b++) tx.data[b] = (uint8_t)(i * 8 + b);
        if (twai_transmit(&tx, pdMS_TO_TICKS(50)) != ESP_OK) continue;
        r.sent++;

        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(50)) == ESP_OK && rx.identifier == TEST_ID &&
            rx.data_length_code == 8 && memcmp(rx.data, tx.data, 8) == 0) {
            r.received++;
        }
    }

    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        r.tx_errors = st.tx_error_counter;
        r.rx_errors = st.rx_error_counter;
        r.bus_off = st.state == TWAI_STATE_BUS_OFF;
    }
    twai_stop();
    twai_driver_uninstall();

    Serial.printf("Test CAN: wyslane %d, odebrane %d, bledy TX %u RX %u%s\n", r.sent, r.received,
                  (unsigned)r.tx_errors, (unsigned)r.rx_errors, r.bus_off ? ", BUS OFF" : "");
    return r.sent > 0 && r.received == r.sent;
}
