#include "can_test.h"
#include <Arduino.h>
#include <algorithm>
#include "driver/twai.h"
#include "soc/twai_struct.h"
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

#define CAR_LISTEN_MS 1500
#define CAR_REPLY_MS 1000
#define CAR_ACK_CHECK_MS 1000
#define CAR_ATTEMPTS 3

static bool driver_up(twai_mode_t mode) {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, mode);
    g.rx_queue_len = 64;
    g.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_BUS_OFF;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }
    return true;
}

static void keep_frame(CanCarResult &r, const twai_message_t &rx) {
    int i = r.frame_count++;
    r.frame_id[i] = rx.identifier;
    r.frame_dlc[i] = rx.data_length_code;
    memcpy(r.frame_data[i], rx.data, 8);
}

static void driver_down(CanCarResult &r) {
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        r.tx_errors = st.tx_error_counter;
        r.rx_errors = st.rx_error_counter;
        r.bus_off = st.state == TWAI_STATE_BUS_OFF;
    }
    twai_stop();
    twai_driver_uninstall();
}

bool can_car_test(CanCarResult &r) {
    memset(&r, 0, sizeof(r));

    // 1) Nasłuch: kontroler nic nie nadaje (nawet potwierdzeń ACK)
    if (!driver_up(TWAI_MODE_LISTEN_ONLY)) return false;
    r.driver_ok = true;
    uint32_t start = millis();
    while (millis() - start < CAR_LISTEN_MS) {
        uint32_t alerts;
        if (twai_read_alerts(&alerts, 0) == ESP_OK && (alerts & TWAI_ALERT_BUS_ERROR)) r.listen_errors++;
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) != ESP_OK) continue;
        if (!r.listen_frames) keep_frame(r, rx); // przykładowa ramka z nasłuchu
        r.listen_frames++;
        bool known = false;
        for (int i = 0; i < r.listen_ids && i < CAR_TEST_IDS; i++) known |= r.ids[i] == rx.identifier;
        if (!known) {
            if (r.listen_ids < CAR_TEST_IDS) r.ids[r.listen_ids] = rx.identifier;
            r.listen_ids++;
        }
    }
    CanCarResult tmp = r;
    driver_down(tmp);
    r.listen_errors += tmp.rx_errors;

    // 2) Zwykły tryb bez zapytań: kontroler tylko potwierdza (ACK) ramki z auta. Jeśli bramka nas
    //    słyszy, przestaje powtarzać niepotwierdzoną ramkę i ich liczba spada do kilkunastu na sekundę.
    if (!driver_up(TWAI_MODE_NORMAL)) return false;
    start = millis();
    while (millis() - start < CAR_ACK_CHECK_MS) {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) r.normal_frames++;
    }

    // 3) Zapytanie odczytu do DDE: 0x6F1 [12 03 22 F1 90] - numer VIN (do 3 prób)
    twai_message_t tx = {};
    tx.identifier = 0x6F1;
    tx.data_length_code = 8;
    tx.ss = 1;
    const uint8_t req[8] = {0x12, 0x03, 0x22, 0xF1, 0x90, 0xFF, 0xFF, 0xFF};
    memcpy(tx.data, req, 8);
    for (int a = 0; a < CAR_ATTEMPTS && !r.tx_acked; a++) {
        twai_message_t rx;
        while (twai_receive(&rx, 0) == ESP_OK) {}
        uint32_t alerts;
        twai_read_alerts(&alerts, 0);
        r.attempts++;
        if (twai_transmit(&tx, pdMS_TO_TICKS(50)) != ESP_OK) continue;
        start = millis();
        while (millis() - start < 50) {
            if (twai_read_alerts(&alerts, pdMS_TO_TICKS(5)) != ESP_OK) continue;
            if (alerts & TWAI_ALERT_TX_SUCCESS) { r.tx_acked = true; break; }
            if (alerts & (TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF)) break;
        }
        if (!r.tx_acked) {
            // Rejestr przechwycenia błędu: rodzaj (bity 7-6), kierunek (5), miejsce w ramce (4-0); 0 = już odczytany
            uint32_t ecc = TWAI.error_code_capture_reg.val;
            if (ecc) {
                r.err_valid = true;
                r.err_type = (ecc >> 6) & 0x3;
                r.err_tx = !((ecc >> 5) & 0x1);
                r.err_seg = ecc & 0x1F;
            }
            delay(20);
        }
    }
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        r.arb_lost = st.arb_lost_count;
        r.bus_errors = st.bus_error_count;
    }
    start = millis();
    while (r.tx_acked && millis() - start < CAR_REPLY_MS) {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) != ESP_OK) continue;
        r.reply_count++;
        if (rx.identifier == 0x612 && rx.data[0] == 0xF1) r.dde_reply = true;
        // Zapisujemy w pierwszej kolejności odpowiedzi diagnostyczne (0x6xx)
        bool diag = (rx.identifier & 0x700) == 0x600;
        if (r.frame_count < CAR_TEST_FRAMES && (diag || r.frame_count < CAR_TEST_FRAMES - 2)) keep_frame(r, rx);
    }
    driver_down(r);

    if (r.err_valid) {
        Serial.printf("Test w aucie: blad typ %u, %s, segment 0x%02X\n", r.err_type, r.err_tx ? "TX" : "RX", r.err_seg);
    }
    Serial.printf("Test w aucie: nasluch %d ramek (%d ID), bledy %u; zwykly tryb %d ramek; proby %d, arbitraz %u, "
                  "bledy %u; ACK %s; po zapytaniu %d ramek, DDE %s\n",
                  r.listen_frames, r.listen_ids, (unsigned)r.listen_errors, r.normal_frames, r.attempts,
                  (unsigned)r.arb_lost, (unsigned)r.bus_errors, r.tx_acked ? "tak" : "NIE", r.reply_count,
                  r.dde_reply ? "ODPOWIADA" : "brak");
    for (int i = 0; i < r.frame_count; i++) {
        Serial.printf("  %03X [%u] %02X %02X %02X %02X %02X %02X %02X %02X\n", (unsigned)r.frame_id[i], r.frame_dlc[i],
                      r.frame_data[i][0], r.frame_data[i][1], r.frame_data[i][2], r.frame_data[i][3],
                      r.frame_data[i][4], r.frame_data[i][5], r.frame_data[i][6], r.frame_data[i][7]);
    }
    return r.dde_reply;
}

bool can_obd2_probe(ObdResult &r) {
    static const uint8_t PIDS[OBD_PIDS] = {0x00, 0x0C, 0x05, 0x0F, 0x0B, 0x42}; // obsługa, obroty, płyn, IAT, MAP, napięcie
    memset(&r, 0, sizeof(r));
    if (!driver_up(TWAI_MODE_NORMAL)) return false;
    delay(30); // bramka przestaje powtarzać niepotwierdzoną ramkę 0x130
    bool any = false;
    for (int i = 0; i < OBD_PIDS; i++) {
        r.pid[i] = PIDS[i];
        twai_message_t rx;
        while (twai_receive(&rx, 0) == ESP_OK) {}
        twai_message_t tx = {};
        tx.identifier = 0x7DF;
        tx.data_length_code = 8;
        const uint8_t req[8] = {0x02, 0x01, PIDS[i], 0x55, 0x55, 0x55, 0x55, 0x55};
        memcpy(tx.data, req, 8);
        if (twai_transmit(&tx, pdMS_TO_TICKS(50)) != ESP_OK) continue;
        uint32_t start = millis();
        while (millis() - start < 300) {
            if (twai_receive(&rx, pdMS_TO_TICKS(10)) != ESP_OK) continue;
            if (rx.identifier < 0x7E8 || rx.identifier > 0x7EF) continue;
            if (rx.data[1] != 0x41 || rx.data[2] != PIDS[i]) continue;
            r.ok[i] = any = true;
            r.from[i] = rx.identifier;
            r.len[i] = rx.data[0] >= 2 ? (uint8_t)std::min<int>(rx.data[0] - 2, 4) : 0;
            memcpy(r.data[i], rx.data + 3, 4);
            break;
        }
        Serial.printf("OBD-II PID %02X: %s\n", PIDS[i], r.ok[i] ? "OK" : "brak");
    }
    CanCarResult tmp;
    driver_down(tmp);
    return any;
}
