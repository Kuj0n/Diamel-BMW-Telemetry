#include "comm.h"
#include <Arduino.h>
#include <math.h>
#include <algorithm>
#include "driver/twai.h"
#include "ble_obd.h"
#include "catalog.h"
#include "config.h"
#include "power.h"
#include "settings.h"
#include "telemetry.h"

#define TESTER_ID 0x6F1
#define TESTER_ADDR 0xF1
#define RESP_BASE 0x600
#define TX_WAIT_MS 30         // potwierdzenie ramki przez inny węzeł (ACK) - na żywej magistrali < 1 ms
#define PENDING_EXTRA_MS 5000 // sterownik odpowiedział "czekaj" (NRC 0x78)
#define VALUE_MAX_AGE_MS 5000 // starsza wartość = brak danych
#define BUS_RETRY_MS 2000     // po braku połączenia - kolejna próba
#define SETTLE_MS 30          // po starcie kontrolera: bramka przestaje powtarzać niepotwierdzoną ramkę
#define SEND_TRIES 3          // ramka przegrała arbitraż albo trafiła na błąd - kolejna próba
#define ACTIVE_MAX (LIVE_PAGES * PAGE_MAX + CHART_SERIES_MAX + 1)
#define RESP_MAX 512

static SemaphoreHandle_t bus_lock;
static UdsRxDebug rx_debug; // pod bus_lock // jedno zapytanie naraz (odpytywanie, odczyt błędów)
static bool driver_on = false;

static volatile bool want_running = false;
static volatile CommState state = COMM_STOPPED;
static char status[96] = "Komunikacja zatrzymana";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static float *values;      // w PSRAM, indeks = ParamIdx
static uint8_t *via_block; // 1 = parametr czytany przez zestaw dynamiczny (sterownik odrzuca zwykły odczyt)
static uint32_t *stamps;   // millis() odczytu, 0 = nigdy
static int values_count;

static void set_status(CommState st, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&mux);
    bool changed = strcmp(buf, status) != 0;
    strlcpy(status, buf, sizeof(status));
    portEXIT_CRITICAL(&mux);
    state = st;
    if (changed) Serial.printf("Komunikacja: %s\n", buf);
}

const char *uds_result_text(UdsResult r) {
    switch (r) {
        case UDS_OK:           return "OK";
        case UDS_NO_BUS:       return "Brak polaczenia z magistrala CAN (nikt nie potwierdza ramek) - wtyczka OBD, zaplon?";
        case UDS_NO_REPLY:     return "Sterownik nie odpowiada (timeout)";
        case UDS_NEGATIVE:     return "Sterownik odrzucil zapytanie";
        case UDS_BLOCKED:      return "Zablokowane - dozwolony tylko odczyt";
        case UDS_NO_TRANSPORT: return "Brak polaczenia z adapterem";
    }
    return "?";
}

// --- Kontroler CAN (TWAI) ---
static bool driver_start() {
    if (driver_on) return true;
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    g.rx_queue_len = 32;
    g.tx_queue_len = 4;
    g.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }
    driver_on = true;
    // Bez testera bramka F-serii powtarza niepotwierdzoną ramkę 0x130 bez przerwy (100% magistrali) -
    // po starcie my ją potwierdzamy i ruch spada; wcześniejsza ramka przegrałaby arbitraż
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {}
    return true;
}

static void driver_stop() {
    if (!driver_on) return;
    twai_stop();
    twai_driver_uninstall();
    driver_on = false;
}

// Ramka jednorazowa (bez powtórzeń): brak potwierdzenia = nikt nie słucha na magistrali
static bool send_frame_once(uint32_t id, const uint8_t *data) {
    uint32_t alerts = 0;
    // Wyczyszczenie starych zdarzeń
    if (twai_read_alerts(&alerts, 0) == ESP_OK && (alerts & TWAI_ALERT_BUS_OFF)) twai_initiate_recovery();
    twai_message_t m = {};
    m.identifier = id;
    m.data_length_code = 8;
    m.ss = 1;
    memcpy(m.data, data, 8);
    if (twai_transmit(&m, pdMS_TO_TICKS(TX_WAIT_MS)) != ESP_OK) return false;
    uint32_t start = millis();
    while (millis() - start < TX_WAIT_MS) {
        if (twai_read_alerts(&alerts, pdMS_TO_TICKS(5)) != ESP_OK) continue;
        if (alerts & TWAI_ALERT_TX_SUCCESS) return true;
        if (alerts & (TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF)) break;
    }
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery(); // po odzyskaniu sterownik trzeba uruchomić od nowa
        driver_stop();
    }
    return false;
}

// Kilka prób: pojedyncza ramka przegrywa arbitraż z ruchem auta (np. 0x130 tuż po starcie)
static bool send_frame(uint32_t id, const uint8_t *data) {
    for (int i = 0; i < SEND_TRIES; i++) {
        if (!driver_start()) return false;
        if (send_frame_once(id, data)) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static bool allowed(const uint8_t *req, size_t len) {
    if (!len) return false;
    if (req[0] == 0x10) return len == 2 && (req[1] == 0x01 || req[1] == 0x03); // sesja zwykła / rozszerzona
    // Zestaw odczytu w RAM sterownika (DDE podaje pomiary tylko tak): zdefiniuj (01) / wyczyść (03),
    // wyłącznie identyfikatory F300-F3FF; znika po wyczyszczeniu, końcu sesji i wyłączeniu zapłonu
    if (req[0] == 0x2C) return len >= 4 && (req[1] == 0x01 || req[1] == 0x03) && req[2] == 0xF3;
    return req[0] == 0x22 || req[0] == 0x19 || req[0] == 0x3E;
}

// Dłuższe zapytanie: ramka pierwsza, czekamy na kontrolę przepływu sterownika, potem ramki kolejne
static bool send_multi(uint8_t ecu, const uint8_t *req, size_t len, uint32_t timeout_ms) {
    uint8_t frame[8];
    memset(frame, 0x00, sizeof(frame));
    frame[0] = ecu;
    frame[1] = 0x10 | (uint8_t)(len >> 8);
    frame[2] = (uint8_t)len;
    memcpy(frame + 3, req, 5);
    if (!send_frame(TESTER_ID, frame)) return false;
    uint8_t st_min = 0, block = 0;
    uint32_t deadline = millis() + timeout_ms;
    twai_message_t rx;
    for (;;) {
        if ((int32_t)(deadline - millis()) <= 0) return false;
        if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;
        if (rx.identifier != RESP_BASE + ecu || rx.data[0] != TESTER_ADDR || (rx.data[1] >> 4) != 3) continue;
        if ((rx.data[1] & 0x0F) == 1) { // sterownik każe czekać
            deadline = millis() + timeout_ms;
            continue;
        }
        if ((rx.data[1] & 0x0F) != 0) return false; // przepełnienie / przerwanie
        block = rx.data[2];
        st_min = rx.data[3] <= 0x7F ? rx.data[3] : 1;
        break;
    }
    size_t sent = 5;
    uint8_t sn = 1, in_block = 0;
    while (sent < len) {
        memset(frame, 0x00, sizeof(frame));
        frame[0] = ecu;
        frame[1] = 0x20 | (sn++ & 0x0F);
        size_t n = std::min<size_t>(6, len - sent);
        memcpy(frame + 2, req + sent, n);
        sent += n;
        if (!send_frame(TESTER_ID, frame)) return false;
        vTaskDelay(pdMS_TO_TICKS(st_min ? st_min : 1));
        if (block && ++in_block >= block && sent < len) { // kolejna kontrola przepływu
            in_block = 0;
            deadline = millis() + timeout_ms;
            for (;;) {
                if ((int32_t)(deadline - millis()) <= 0) return false;
                if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;
                if (rx.identifier == RESP_BASE + ecu && rx.data[0] == TESTER_ADDR && rx.data[1] == 0x30) break;
            }
        }
    }
    return true;
}

// ISO-TP z adresowaniem rozszerzonym: [adres][PCI][dane...]
static UdsResult can_request(uint8_t ecu, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, size_t *resp_len,
                             uint32_t timeout_ms) {
    if (len > 128) return UDS_BLOCKED;
    if (!driver_start()) return UDS_NO_BUS;
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {} // stare ramki z kolejki

    if (len > 6) {
        if (!send_multi(ecu, req, len, timeout_ms)) return UDS_NO_REPLY;
    } else {
        uint8_t frame[8];
        memset(frame, 0xFF, sizeof(frame));
        frame[0] = ecu;
        frame[1] = (uint8_t)len; // ramka pojedyncza
        memcpy(frame + 2, req, len);
        if (!send_frame(TESTER_ID, frame)) return UDS_NO_BUS;
    }

    size_t total = 0, got = 0;
    uint32_t deadline = millis() + timeout_ms;
    rx_debug.frames = 0;
    while ((int32_t)(deadline - millis()) > 0) {
        if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;
        rx_debug.frames++;
        rx_debug.last_id = rx.identifier;
        memcpy(rx_debug.last, rx.data, 8);
        if (rx.identifier != RESP_BASE + ecu || rx.data_length_code < 2 || rx.data[0] != TESTER_ADDR) continue;
        uint8_t pci = rx.data[1] >> 4;
        if (pci == 0) { // ramka pojedyncza
            size_t n = std::min<size_t>(rx.data[1] & 0x0F, std::min<size_t>(6, cap));
            memcpy(resp, rx.data + 2, n);
            if (n >= 3 && resp[0] == 0x7F && resp[2] == 0x78) { // "odpowiedź w przygotowaniu" - czekamy dłużej
                deadline = millis() + PENDING_EXTRA_MS;
                continue;
            }
            *resp_len = n;
            return n && resp[0] == 0x7F ? UDS_NEGATIVE : UDS_OK;
        }
        if (pci == 1) { // pierwsza ramka dłuższej odpowiedzi
            total = std::min<size_t>(((rx.data[1] & 0x0F) << 8) | rx.data[2], cap);
            got = std::min<size_t>(5, total);
            memcpy(resp, rx.data + 3, got);
            uint8_t fc[8];
            memset(fc, 0xFF, sizeof(fc));
            fc[0] = ecu;
            fc[1] = 0x30; // kontrola przepływu: wysyłaj wszystko, bez przerw
            fc[2] = 0x00;
            fc[3] = 0x00;
            if (!send_frame(TESTER_ID, fc)) return UDS_NO_BUS;
            deadline = millis() + timeout_ms;
            continue;
        }
        if (pci == 2 && total) { // ramki kolejne
            size_t n = std::min<size_t>(6, total - got);
            memcpy(resp + got, rx.data + 2, n);
            got += n;
            if (got >= total) {
                *resp_len = total;
                return resp[0] == 0x7F ? UDS_NEGATIVE : UDS_OK;
            }
        }
    }
    return UDS_NO_REPLY;
}

UdsResult uds_request(uint8_t ecu, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, size_t *resp_len,
                      uint32_t timeout_ms) {
    *resp_len = 0;
    if (!allowed(req, len)) {
        Serial.printf("Komunikacja: ZABLOKOWANO usluge 0x%02X (tylko odczyt)\n", len ? req[0] : 0);
        return UDS_BLOCKED;
    }
    if (g_settings.data_source == SRC_BLE) {
        // Odczyt przez adapter ELM327 - jeszcze nie zaimplementowany
        return UDS_NO_TRANSPORT;
    }
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    UdsResult r = can_request(ecu, req, len, resp, cap, resp_len, timeout_ms);
    xSemaphoreGive(bus_lock);
    return r;
}

static UdsResult can_obd(uint8_t pid, uint8_t *resp, size_t cap, size_t *resp_len, uint32_t timeout_ms) {
    if (!driver_start()) return UDS_NO_BUS;
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {}
    const uint8_t frame[8] = {0x02, 0x01, pid, 0x55, 0x55, 0x55, 0x55, 0x55};
    if (!send_frame(0x7DF, frame)) return UDS_NO_BUS;
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(deadline - millis()) > 0) {
        if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;
        if (rx.identifier < 0x7E8 || rx.identifier > 0x7EF) continue;
        uint8_t n = rx.data[0];
        if (n > 7 || n < 2) continue; // tylko ramki pojedyncze (tryb 01 mieści się w jednej)
        if (rx.data[1] == 0x7F && rx.data[2] == 0x01) { // odmowa
            *resp_len = std::min<size_t>(3, cap);
            memcpy(resp, rx.data + 1, *resp_len);
            return UDS_NEGATIVE;
        }
        if (rx.data[1] != 0x41 || rx.data[2] != pid) continue;
        *resp_len = std::min<size_t>(n, cap);
        memcpy(resp, rx.data + 1, *resp_len);
        return UDS_OK;
    }
    return UDS_NO_REPLY;
}

UdsResult obd_request(uint8_t pid, uint8_t *resp, size_t cap, size_t *resp_len, uint32_t timeout_ms) {
    *resp_len = 0;
    if (g_settings.data_source == SRC_BLE) return UDS_NO_TRANSPORT;
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    UdsResult r = can_obd(pid, resp, cap, resp_len, timeout_ms);
    xSemaphoreGive(bus_lock);
    return r;
}

void uds_last_rx(UdsRxDebug &out) {
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    out = rx_debug;
    xSemaphoreGive(bus_lock);
}

static uint32_t *block_sig(uint8_t addr);

#define DDE_ADDR 0x12
#define CSF_NUMID 4  // pozycje w rekordzie CSF liczone od bajtu usługi (tabela CSF_WERTE)
#define CSF_TLRG 16
#define CSF_MIN_LEN 20

static bool csf_changeable(size_t i) {
    return i == CSF_NUMID || i == CSF_NUMID + 1 || (i >= CSF_TLRG && i < CSF_TLRG + 4);
}

UdsResult uds_dpf_regen(char *msg, size_t len) {
    if (g_settings.data_source != SRC_CAN_DIRECT) {
        snprintf(msg, len, "Dostepne tylko przy polaczeniu CAN");
        return UDS_NO_TRANSPORT;
    }
    static uint8_t before[160], req[160], after[160];
    size_t nb = 0, na = 0, l = 0;
    const uint8_t rd[3] = {0x22, 0x5F, 0xB4};
    const uint8_t ext[2] = {0x10, 0x03}, def[2] = {0x10, 0x01};
    uint8_t tmp[16];
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    UdsResult r = can_request(DDE_ADDR, rd, 3, before, sizeof(before), &nb, 1000);
    // Rekord musi wyglądać jak w zadaniu BMW: 62 5F B4 04 ...
    if (r != UDS_OK || nb < CSF_MIN_LEN || nb > sizeof(req) || before[0] != 0x62 || before[1] != 0x5F ||
        before[2] != 0xB4 || before[3] != 0x04) {
        xSemaphoreGive(bus_lock);
        snprintf(msg, len, "Nie odczytano rekordu DPF (5FB4): %s", r == UDS_OK ? "nieznany format" : uds_result_text(r));
        return r == UDS_OK ? UDS_NEGATIVE : r;
    }
    memcpy(req, before, nb);
    req[0] = 0x2E;
    req[CSF_NUMID] = 0x06;     // SERVICEFALLNR 6 = EEPROM (pojedyncza wartość)
    req[CSF_NUMID + 1] = 0x07; // ABID 07 = TLRG
    memset(req + CSF_TLRG, 0, 4);
    r = can_request(DDE_ADDR, req, nb, tmp, sizeof(tmp), &l, 3000);
    bool session = false;
    if (r == UDS_NEGATIVE && l >= 3 && (tmp[2] == 0x7F || tmp[2] == 0x22)) {
        // Zapis niedostępny w zwykłej sesji - jeszcze raz w rozszerzonej
        size_t sl = 0;
        if (can_request(DDE_ADDR, ext, 2, tmp, sizeof(tmp), &sl, 1000) == UDS_OK) {
            session = true;
            r = can_request(DDE_ADDR, req, nb, tmp, sizeof(tmp), &l, 3000);
        }
    }
    uint8_t nrc = r == UDS_NEGATIVE && l >= 3 ? tmp[2] : 0;
    bool written = r == UDS_OK && l >= 1 && tmp[0] == 0x6E;
    UdsResult rr = can_request(DDE_ADDR, rd, 3, after, sizeof(after), &na, 1000);
    if (session) {
        size_t sl = 0;
        can_request(DDE_ADDR, def, 2, tmp, sizeof(tmp), &sl, 1000);
    }
    xSemaphoreGive(bus_lock);

    // Kontrola: poza NUMID i TLRG rekord ma być taki sam jak przed zapisem
    int changed = -1;
    if (rr == UDS_OK && na == nb) {
        changed = 0;
        for (size_t i = 3; i < nb; i++) {
            if (!csf_changeable(i) && after[i] != before[i]) changed++;
        }
    }
    Serial.printf("Regeneracja DPF: zapis %s (kod 0x%02X), %u B, zmienione inne bajty: %d\n",
                  written ? "OK" : uds_result_text(r), nrc, (unsigned)nb, changed);
    if (!written) {
        if (nrc) snprintf(msg, len, "DDE odmowilo (kod 0x%02X)%s", nrc, nrc == 0x33 ? " - wymaga zabezpieczenia" : "");
        else snprintf(msg, len, "Zapis nieudany: %s", uds_result_text(r));
        return r == UDS_OK ? UDS_NO_REPLY : r;
    }
    if (changed < 0) snprintf(msg, len, "Zgloszono regeneracje (nie udalo sie sprawdzic rekordu)");
    else if (changed > 0) snprintf(msg, len, "Zgloszono regeneracje - UWAGA: zmienilo sie %d innych bajtow rekordu", changed);
    else snprintf(msg, len, "Zgloszono regeneracje - DDE wypali filtr, gdy warunki beda spelnione");
    return UDS_OK;
}

UdsResult uds_clear_dtc(uint8_t ecu, uint8_t *nrc) {
    *nrc = 0;
    if (g_settings.data_source != SRC_CAN_DIRECT) return UDS_NO_TRANSPORT;
    static const uint8_t clr[4] = {0x14, 0xFF, 0xFF, 0xFF};
    static uint8_t resp[16];
    size_t len = 0;
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    UdsResult r = can_request(ecu, clr, 4, resp, sizeof(resp), &len, 3000);
    if (r == UDS_NEGATIVE && len >= 3 && (resp[2] == 0x7F || resp[2] == 0x22)) {
        // Usługa niedostępna w zwykłej sesji - jeszcze raz w rozszerzonej, potem powrót
        const uint8_t ext[2] = {0x10, 0x03}, def[2] = {0x10, 0x01};
        size_t l = 0;
        if (can_request(ecu, ext, 2, resp, sizeof(resp), &l, 1000) == UDS_OK) {
            r = can_request(ecu, clr, 4, resp, sizeof(resp), &len, 3000);
            can_request(ecu, def, 2, resp + 8, sizeof(resp) - 8, &l, 1000);
        }
    }
    if (r == UDS_NEGATIVE && len >= 3) *nrc = resp[2];
    if (r == UDS_OK && (len < 1 || resp[0] != 0x54)) r = UDS_NO_REPLY;
    xSemaphoreGive(bus_lock);
    Serial.printf("Kasowanie bledow 0x%02X: %s (kod 0x%02X)\n", ecu, uds_result_text(r), *nrc);
    return r;
}

UdsResult uds_block_probe(uint8_t ecu, const uint16_t *dids, const uint8_t *sizes, int n, uint8_t *resp, size_t cap,
                          size_t *len) {
    if (n < 1 || n > 20) return UDS_BLOCKED;
    const uint8_t clr[4] = {0x2C, 0x03, 0xF3, 0x03};
    uint8_t def[4 + 20 * 4] = {0x2C, 0x01, 0xF3, 0x03};
    for (int k = 0; k < n; k++) {
        def[4 + k * 4] = dids[k] >> 8;
        def[5 + k * 4] = (uint8_t)dids[k];
        def[6 + k * 4] = 1;
        def[7 + k * 4] = sizes[k];
    }
    const uint8_t rd[3] = {0x22, 0xF3, 0x03};
    size_t l = 0;
    uds_request(ecu, clr, 4, resp, cap, &l, 500);
    UdsResult r = uds_request(ecu, def, 4 + n * 4, resp, cap, len, 500);
    if (r == UDS_OK) r = uds_request(ecu, rd, 3, resp, cap, len, 500);
    uint8_t tmp[8];
    uds_request(ecu, clr, 4, tmp, sizeof(tmp), &l, 500);
    *block_sig(ecu) = 0; // odpytywanie zdefiniuje swój zestaw od nowa
    return r;
}

bool uds_release() {
    if (want_running) return false;
    xSemaphoreTake(bus_lock, portMAX_DELAY);
    driver_stop();
    xSemaphoreGive(bus_lock);
    return true;
}

// --- Dekodowanie wartości z odpowiedzi 62 DID_H DID_L dane... ---
static bool decode(const ParamDef &p, const uint8_t *data, size_t len, float &out) {
    static const uint8_t SIZES[] = {1, 1, 2, 2, 4, 4, 4};
    size_t size = SIZES[p.type];
    if (p.pos + size > len) return false;
    uint32_t raw = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = p.big_endian ? data[p.pos + i] : data[p.pos + size - 1 - i];
        raw = (raw << 8) | b;
    }
    // Same bajty FF = "brak wartości" u sterowników BMW (np. 40.000 V z U8 = 255). Pomijamy bitowe
    // (maska), opisowe (tabela tekstów) i procenty (255 bywa 100%). Liczby ze znakiem: FF..FF = -1, prawidłowe.
    static const uint32_t ALL_FF[] = {0xFF, 0xFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    bool is_unsigned = p.type == PTYPE_U8 || p.type == PTYPE_U16 || p.type == PTYPE_U32;
    if (is_unsigned && !p.mask && p.texts < 0 && raw == ALL_FF[p.type] && !(p.unit && strcmp(p.unit, "%") == 0)) return false;
    if (p.mask) {
        raw &= p.mask;
        raw >>= __builtin_ctz(p.mask);
    }
    float v;
    switch (p.type) {
        case PTYPE_S8:  v = p.mask ? raw : (int8_t)raw; break;
        case PTYPE_S16: v = p.mask ? raw : (int16_t)raw; break;
        case PTYPE_S32: v = p.mask ? raw : (int32_t)raw; break;
        case PTYPE_F32: memcpy(&v, &raw, 4); break;
        default:        v = raw; break;
    }
    out = v * p.factor + p.offset;
    return isfinite(out);
}

// Parametry do odpytywania: wszystkie strony Odczytów + wykres + parametr usypiania, tylko z katalogu auta
static volatile int view = VIEW_NONE;
#define WEB_WATCH_MAX (PAGE_MAX + CHART_SERIES_MAX)
#define WEB_WATCH_MS 3000
static uint16_t web_ids[WEB_WATCH_MAX];
static volatile int web_count = 0;
static volatile uint32_t web_ms = 0;

void comm_set_view(int v) { view = v; }

#define VIEW_LIST_MAX 20
static uint16_t view_list[VIEW_LIST_MAX];
static volatile int view_list_n = 0;

void comm_set_view_list(const uint16_t *ids, int n) {
    n = std::min(n, VIEW_LIST_MAX);
    portENTER_CRITICAL(&mux);
    memcpy(view_list, ids, n * sizeof(uint16_t));
    view_list_n = n;
    portEXIT_CRITICAL(&mux);
}

// Szybkość: cykle z odpowiedziami w ostatnich ~2 s
static volatile uint16_t rate_x10 = 0;
static uint32_t rate_ms = 0, rate_cycles = 0;

static void rate_count(bool ok) {
    if (ok) rate_cycles++;
    uint32_t now = millis();
    if (now - rate_ms >= 2000) {
        rate_x10 = rate_ms ? (uint16_t)(rate_cycles * 10000 / (now - rate_ms)) : 0;
        rate_ms = now;
        rate_cycles = 0;
    }
}

uint16_t comm_rate_x10() { return want_running ? rate_x10 : 0; }

void comm_web_watch(const uint16_t *ids, int n) {
    n = std::min(n, WEB_WATCH_MAX);
    portENTER_CRITICAL(&mux);
    memcpy(web_ids, ids, n * sizeof(uint16_t));
    web_count = n;
    web_ms = millis() | 1;
    portEXIT_CRITICAL(&mux);
}

// Tylko to, co ktoś ogląda albo zapisuje - bez odpytywania stron, których nie widać
static int collect_active(ParamIdx *out) {
    int n = 0;
    auto add = [&](ParamIdx id) {
        if (id == PARAM_NONE || id < P_COUNT || id >= params_count() || n >= ACTIVE_MAX) return;
        for (int i = 0; i < n; i++) {
            if (out[i] == id) return;
        }
        out[n++] = id;
    };
    auto add_page = [&](int p) {
        for (int i = 0; i < g_settings.page_count[p]; i++) add(g_settings.page_ids[p][i]);
    };
    auto add_chart = [&]() {
        for (int i = 0; i < CHART_SERIES_MAX; i++) add(g_settings.chart_series[i]);
    };
    int v = view;
    if (v >= 0 && v < LIVE_PAGES) add_page(v);
    if (v == VIEW_CHART) add_chart();
    if (v == VIEW_LIST) {
        uint16_t ids[VIEW_LIST_MAX];
        int ln;
        portENTER_CRITICAL(&mux);
        ln = view_list_n;
        memcpy(ids, view_list, ln * sizeof(uint16_t));
        portEXIT_CRITICAL(&mux);
        for (int i = 0; i < ln; i++) add(ids[i]);
    }
    if (g_settings.sleep_enabled) add(g_settings.sleep_param);
    uint16_t ids[WEB_WATCH_MAX];
    int wn = 0;
    portENTER_CRITICAL(&mux);
    if (web_ms && millis() - web_ms < WEB_WATCH_MS) {
        wn = web_count;
        memcpy(ids, web_ids, wn * sizeof(uint16_t));
    }
    portEXIT_CRITICAL(&mux);
    for (int i = 0; i < wn; i++) add(ids[i]);
    return n;
}

// --- Zestaw dynamiczny (UDS 2C): sterownik silnika B47 podaje pomiary tylko tak ---
// 2C 01 F3 03 [DID_H DID_L pozycja(od 1) rozmiar]... definiuje zestaw F303 w RAM sterownika,
// 22 F3 03 zwraca wszystkie wartości po kolei. Zestaw zostaje do zmiany sesji / wyłączenia zapłonu,
// więc definiujemy go tylko, gdy zmienia się lista parametrów albo sterownik go "zgubi".
#define BLOCK_DID 0xF303
#define BLOCK_MAX 20 // elementów w zestawie (zapytanie 84 B). Jeden zestaw na sterownik, definiowany raz -
                     // ciągłe przedefiniowywanie (kilka zestawów na zmianę) zawiesiło diagnostykę DDE
#define BLOCK_CACHE 8

struct BlockCache {
    uint8_t addr;
    uint32_t sig; // podpis listy elementów; 0 = nic nie zdefiniowano
};
static BlockCache block_cache[BLOCK_CACHE];

static uint32_t *block_sig(uint8_t addr) {
    for (BlockCache &c : block_cache) {
        if (c.sig && c.addr == addr) return &c.sig;
    }
    for (BlockCache &c : block_cache) {
        if (!c.sig) {
            c.addr = addr;
            return &c.sig;
        }
    }
    block_cache[0] = {addr, 0};
    return &block_cache[0].sig;
}

static uint8_t param_size(const ParamDef &p) {
    static const uint8_t SIZES[] = {1, 1, 2, 2, 4, 4, 4};
    return SIZES[p.type];
}

// Jeden zestaw: do BLOCK_MAX parametrów jednego sterownika. Zwraca wynik odczytu.
static UdsResult read_block(uint8_t addr, const ParamIdx *ids, int count, uint8_t *resp, size_t cap) {
    struct Elem {
        uint16_t did;
        uint8_t pos, size;
    } el[BLOCK_MAX];
    int ne = 0;
    int where[BLOCK_MAX]; // element dla każdego parametru
    uint32_t sig = 2166136261u;
    for (int i = 0; i < count; i++) {
        const ParamDef &p = param(ids[i]);
        Elem e = {p.did, (uint8_t)(p.pos + 1), param_size(p)};
        int k = 0;
        while (k < ne && !(el[k].did == e.did && el[k].pos == e.pos && el[k].size == e.size)) k++;
        if (k == ne) {
            el[ne++] = e;
            sig = (sig ^ ((uint32_t)e.did << 16 | e.pos << 8 | e.size)) * 16777619u;
        }
        where[i] = k;
    }
    uint32_t *cached = block_sig(addr);
    size_t len = 0;
    const uint8_t rd[3] = {0x22, BLOCK_DID >> 8, BLOCK_DID & 0xFF};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (*cached != sig) {
            const uint8_t clr[4] = {0x2C, 0x03, BLOCK_DID >> 8, BLOCK_DID & 0xFF};
            uds_request(addr, clr, 4, resp, cap, &len, g_settings.poll_timeout_ms);
            uint8_t def[4 + BLOCK_MAX * 4] = {0x2C, 0x01, BLOCK_DID >> 8, BLOCK_DID & 0xFF};
            for (int k = 0; k < ne; k++) {
                def[4 + k * 4] = el[k].did >> 8;
                def[5 + k * 4] = (uint8_t)el[k].did;
                def[6 + k * 4] = el[k].pos;
                def[7 + k * 4] = el[k].size;
            }
            UdsResult r = uds_request(addr, def, 4 + ne * 4, resp, cap, &len, g_settings.poll_timeout_ms);
            if (r != UDS_OK) {
                *cached = 0;
                return r;
            }
            *cached = sig;
        }
        UdsResult r = uds_request(addr, rd, 3, resp, cap, &len, g_settings.poll_timeout_ms);
        if (r == UDS_NEGATIVE && attempt == 0) { // zestaw zniknął (np. restart sterownika) - definiujemy od nowa
            *cached = 0;
            continue;
        }
        if (r != UDS_OK || len < 3 || resp[0] != 0x62) return r == UDS_OK ? UDS_NO_REPLY : r;
        // Wartości elementów są w odpowiedzi kolejno
        int offset[BLOCK_MAX];
        int o = 3;
        for (int k = 0; k < ne; k++) {
            offset[k] = o;
            o += el[k].size;
        }
        for (int i = 0; i < count; i++) {
            const Elem &e = el[where[i]];
            if (offset[where[i]] + e.size > (int)len) continue;
            ParamDef tmp = param(ids[i]);
            tmp.pos = 0;
            float v;
            if (decode(tmp, resp + offset[where[i]], e.size, v)) {
                values[ids[i]] = v;
                stamps[ids[i]] = millis() | 1;
            }
        }
        return UDS_OK;
    }
    return UDS_NO_REPLY;
}

// Sterownik, który przestał odpowiadać, dostaje przerwę zamiast ciągłych ponowień
#define MUTE_AFTER 3        // tyle braków odpowiedzi z rzędu
#define MUTE_MS 10000       // przerwa dla sterownika
static uint8_t ecu_fails[256];
static uint32_t ecu_mute_until[256];

static bool ecu_muted(uint8_t addr) {
    return ecu_mute_until[addr] && (int32_t)(ecu_mute_until[addr] - millis()) > 0;
}

static void ecu_result(uint8_t addr, UdsResult r) {
    if (r != UDS_NO_REPLY) {
        ecu_fails[addr] = 0;
        ecu_mute_until[addr] = 0;
        return;
    }
    if (++ecu_fails[addr] < MUTE_AFTER) return;
    ecu_fails[addr] = 0;
    ecu_mute_until[addr] = (millis() + MUTE_MS) | 1;
    *block_sig(addr) = 0; // po przerwie zestaw definiujemy od nowa (sterownik mógł się zrestartować)
    Serial.printf("Komunikacja: sterownik 0x%02X nie odpowiada - przerwa %u s\n", addr, MUTE_MS / 1000);
}

static void poll_cycle() {
    ParamIdx active[ACTIVE_MAX];
    int n = collect_active(active);
    if (!n) {
        set_status(COMM_RUNNING, "Czekam - na tym ekranie nie ma parametrow z auta");
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }
    static uint8_t resp[RESP_MAX];
    bool done[ACTIVE_MAX] = {false};
    int requests = 0, ok = 0, refused = 0;
    uint8_t silent_ecu = 0, nrc = 0;
    uint32_t t0 = millis();
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        if (via_block && via_block[active[i]]) continue;
        const ParamDef &p = param(active[i]);
        bool obd = p.addr == OBD_ADDR;
        if (ecu_muted(p.addr)) {
            silent_ecu = p.addr;
            requests++;
            continue;
        }
        uint8_t req[3] = {0x22, (uint8_t)(p.did >> 8), (uint8_t)p.did};
        size_t len;
        UdsResult r = obd ? obd_request((uint8_t)p.did, resp, sizeof(resp), &len, g_settings.poll_timeout_ms)
                          : uds_request(p.addr, req, 3, resp, sizeof(resp), &len, g_settings.poll_timeout_ms);
        requests++;
        ecu_result(p.addr, r);
        // Dane po nagłówku: UDS "62 DID_H DID_L", OBD-II "41 PID"
        size_t head = obd ? 2 : 3;
        bool good = r == UDS_OK && len >= head && resp[0] == (obd ? 0x41 : 0x62);
        if (r == UDS_NO_BUS || r == UDS_NO_TRANSPORT) {
            set_status(COMM_ERROR, "%s", uds_result_text(r));
            vTaskDelay(pdMS_TO_TICKS(BUS_RETRY_MS));
            return;
        }
        // Jedna odpowiedź DID zasila wszystkie parametry z tego DID
        for (int k = i; k < n; k++) {
            const ParamDef &q = param(active[k]);
            if (q.addr != p.addr || q.did != p.did) continue;
            done[k] = true;
            float v;
            if (good && decode(q, resp + head, len - head, v)) {
                values[active[k]] = v;
                stamps[active[k]] = millis() | 1;
            }
        }
        if (r == UDS_OK) ok++;
        else silent_ecu = p.addr;
        if (r == UDS_NEGATIVE) {
            nrc = len >= 3 ? resp[2] : 0;
            if (!obd && nrc == 0x31 && via_block) {
                // "Poza zakresem": spróbujemy przez zestaw dynamiczny (od następnego cyklu)
                for (int k = i; k < n; k++) {
                    const ParamDef &q = param(active[k]);
                    if (q.addr == p.addr && q.did == p.did) via_block[active[k]] = 1;
                }
                requests--;
            } else {
                refused++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_settings.poll_interval_ms));
    }
    // Parametry z zestawów dynamicznych: jeden zestaw na sterownik (do BLOCK_MAX), nadmiar pomijamy -
    // nigdy kilku zestawów na zmianę w jednym cyklu
    bool taken[ACTIVE_MAX] = {false};
    int over = 0;
    for (int i = 0; via_block && i < n; i++) {
        if (!via_block[active[i]] || taken[i]) continue;
        uint8_t addr = param(active[i]).addr;
        ParamIdx group[BLOCK_MAX];
        int g = 0;
        for (int k = i; k < n; k++) {
            if (!via_block[active[k]] || taken[k] || param(active[k]).addr != addr) continue;
            taken[k] = true;
            if (g < BLOCK_MAX) group[g++] = active[k];
            else over++;
        }
        requests++;
        if (ecu_muted(addr)) {
            silent_ecu = addr;
            continue;
        }
        UdsResult r = read_block(addr, group, g, resp, sizeof(resp));
        ecu_result(addr, r);
        if (r == UDS_NO_BUS || r == UDS_NO_TRANSPORT) {
            set_status(COMM_ERROR, "%s", uds_result_text(r));
            vTaskDelay(pdMS_TO_TICKS(BUS_RETRY_MS));
            return;
        }
        if (r == UDS_OK) {
            ok++;
        } else {
            silent_ecu = addr;
            if (r == UDS_NEGATIVE) {
                refused++;
                nrc = resp[2];
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_settings.poll_interval_ms));
    }
    uint32_t dt = millis() - t0;
    rate_count(ok > 0);
    if (!requests) return;
    if (ok == requests && over) {
        set_status(COMM_ERROR, "Za duzo parametrow z jednego sterownika: pominieto %d (max %d)", over, BLOCK_MAX);
    } else if (ok == requests) {
        set_status(COMM_RUNNING, "Polaczono: %d zapytan, cykl %u ms", requests, (unsigned)dt);
    } else if (refused == requests) {
        set_status(COMM_ERROR, "Sterownik 0x%02X odrzuca parametry (kod 0x%02X)", silent_ecu, nrc);
    } else if (ok == 0 && refused == 0) {
        set_status(COMM_ERROR, "Sterowniki nie odpowiadaja (timeout) - zaplon wlaczony?");
    } else if (ok == 0) {
        set_status(COMM_ERROR, "Brak wartosci: odrzucone %d, bez odpowiedzi %d z %d", refused, requests - refused,
                   requests);
    } else {
        if (refused) {
            set_status(COMM_ERROR, "Sterownik 0x%02X odrzuca czesc parametrow (odpowiedzi %d z %d)", silent_ecu, ok,
                       requests);
        } else {
            set_status(COMM_ERROR, "Sterownik 0x%02X nie odpowiada (odpowiedzi %d z %d)", silent_ecu, ok, requests);
        }
    }
}

static void comm_task(void *) {
    for (;;) {
        if (!want_running || power_sleeping()) {
            if (!want_running && state != COMM_STOPPED) set_status(COMM_STOPPED, "Komunikacja zatrzymana");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (g_settings.data_source == SRC_DEMO) {
            set_status(COMM_STOPPED, "Tryb DEMO - komunikacja z autem nieaktywna");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (g_settings.data_source == SRC_BLE && ble_state() != BLE_CONNECTED) {
            set_status(COMM_ERROR, "Brak polaczenia z adapterem BLE");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (g_settings.data_source == SRC_BLE) {
            set_status(COMM_ERROR, "Odczyt przez adapter BLE - w przygotowaniu");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        poll_cycle();
    }
}

void comm_init() {
    bus_lock = xSemaphoreCreateMutex();
    values_count = params_count();
    values = (float *)heap_caps_calloc(values_count, sizeof(float), MALLOC_CAP_SPIRAM);
    stamps = (uint32_t *)heap_caps_calloc(values_count, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    via_block = (uint8_t *)heap_caps_calloc(values_count, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(comm_task, "COMM", 6144, NULL, 3, NULL, 0);
    if (g_settings.comm_autostart) comm_start();
}

void comm_start() {
    want_running = true;
    if (g_settings.data_source != SRC_DEMO) set_status(COMM_RUNNING, "Laczenie...");
}

void comm_stop() {
    want_running = false;
}

bool comm_running() { return want_running; }
CommState comm_state() { return state; }

void comm_status_text(char *buf, size_t len) {
    portENTER_CRITICAL(&mux);
    strlcpy(buf, status, len);
    portEXIT_CRITICAL(&mux);
}

bool comm_value(uint16_t idx, float &out) {
    if (!values || idx >= values_count || !stamps[idx] || !want_running) return false;
    if (millis() - stamps[idx] > VALUE_MAX_AGE_MS) return false;
    out = values[idx];
    return true;
}
