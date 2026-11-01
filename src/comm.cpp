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
#define ACTIVE_MAX (LIVE_MAX + CHART_SERIES_MAX + 1)
#define RESP_MAX 512

static SemaphoreHandle_t bus_lock; // jedno zapytanie naraz (odpytywanie, odczyt błędów)
static bool driver_on = false;

static volatile bool want_running = false;
static volatile CommState state = COMM_STOPPED;
static char status[96] = "Komunikacja zatrzymana";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static float *values;      // w PSRAM, indeks = ParamIdx
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
    return true;
}

static void driver_stop() {
    if (!driver_on) return;
    twai_stop();
    twai_driver_uninstall();
    driver_on = false;
}

// Ramka jednorazowa (bez powtórzeń): brak potwierdzenia = nikt nie słucha na magistrali
static bool send_frame(uint32_t id, const uint8_t *data) {
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

static bool allowed(const uint8_t *req, size_t len) {
    if (!len) return false;
    return req[0] == 0x22 || req[0] == 0x19 || req[0] == 0x3E;
}

// ISO-TP z adresowaniem rozszerzonym: [adres][PCI][dane...]; zapytania mieszczą się w jednej ramce
static UdsResult can_request(uint8_t ecu, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, size_t *resp_len,
                             uint32_t timeout_ms) {
    if (len > 6) return UDS_BLOCKED;
    if (!driver_start()) return UDS_NO_BUS;
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {} // stare ramki z kolejki

    uint8_t frame[8];
    memset(frame, 0xFF, sizeof(frame));
    frame[0] = ecu;
    frame[1] = (uint8_t)len; // ramka pojedyncza
    memcpy(frame + 2, req, len);
    if (!send_frame(TESTER_ID, frame)) return UDS_NO_BUS;

    size_t total = 0, got = 0;
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(deadline - millis()) > 0) {
        if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;
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

// Parametry do odpytywania: Odczyty + wykres + parametr usypiania, tylko z katalogu auta
static int collect_active(ParamIdx *out) {
    int n = 0;
    auto add = [&](ParamIdx id) {
        if (id == PARAM_NONE || id < P_COUNT || id >= params_count()) return;
        for (int i = 0; i < n; i++) {
            if (out[i] == id) return;
        }
        out[n++] = id;
    };
    for (int i = 0; i < g_settings.live_count; i++) add(g_settings.live_ids[i]);
    for (int i = 0; i < CHART_SERIES_MAX; i++) add(g_settings.chart_series[i]);
    if (g_settings.sleep_enabled) add(g_settings.sleep_param);
    return n;
}

static void poll_cycle() {
    ParamIdx active[ACTIVE_MAX];
    int n = collect_active(active);
    if (!n) {
        set_status(COMM_RUNNING, "Brak wybranych parametrow z auta (Ustawienia > Odczyty)");
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }
    static uint8_t resp[RESP_MAX];
    bool done[ACTIVE_MAX] = {false};
    int requests = 0, ok = 0;
    uint8_t silent_ecu = 0;
    uint32_t t0 = millis();
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        const ParamDef &p = param(active[i]);
        uint8_t req[3] = {0x22, (uint8_t)(p.did >> 8), (uint8_t)p.did};
        size_t len;
        UdsResult r = uds_request(p.addr, req, 3, resp, sizeof(resp), &len, g_settings.poll_timeout_ms);
        requests++;
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
            if (r == UDS_OK && len >= 3 && resp[0] == 0x62 && decode(q, resp + 3, len - 3, v)) {
                values[active[k]] = v;
                stamps[active[k]] = millis() | 1;
            }
        }
        if (r == UDS_OK) ok++;
        else silent_ecu = p.addr;
        vTaskDelay(pdMS_TO_TICKS(g_settings.poll_interval_ms));
    }
    uint32_t dt = millis() - t0;
    if (ok == requests) {
        set_status(COMM_RUNNING, "Polaczono: %d zapytan, cykl %u ms", requests, (unsigned)dt);
    } else if (ok == 0) {
        set_status(COMM_ERROR, "Sterowniki nie odpowiadaja (timeout) - zaplon wlaczony?");
    } else {
        set_status(COMM_ERROR, "Sterownik 0x%02X nie odpowiada (odpowiedzi %d z %d)", silent_ecu, ok, requests);
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
