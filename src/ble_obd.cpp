#include "ble_obd.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <algorithm>

#define SCAN_SECONDS 5
#define CONNECT_TIMEOUT_MS 8000
#define RX_BUF_SIZE 1024
#define WRITE_CHUNK 20 // najmniejszy MTU BLE (23) minus nagłówek - działa z każdym adapterem

enum ReqType : uint8_t { REQ_SCAN, REQ_CONNECT, REQ_DISCONNECT, REQ_TEST };
struct Request {
    ReqType type;
    char addr[18];
    uint8_t addr_type;
};

static TaskHandle_t task;
static QueueHandle_t queue;
static SemaphoreHandle_t cmd_lock; // jedna komenda ELM327 naraz
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile BleState state = BLE_OFF;
static BleFound found[BLE_FOUND_MAX];
static int found_count = 0;
static char status_text[64] = "Bluetooth wylaczony";
static char test_text[200] = "";

static BLEClient *client;
static BLERemoteCharacteristic *tx_char;
static bool tx_with_response;
static char rx_buf[RX_BUF_SIZE];
static size_t rx_len = 0;

static void set_text(char *dst, size_t len, const char *src) {
    portENTER_CRITICAL(&mux);
    strlcpy(dst, src, len);
    portEXIT_CRITICAL(&mux);
}

static void set_status(const char *text) {
    set_text(status_text, sizeof(status_text), text);
    Serial.printf("BLE: %s\n", text);
}

// Odpowiedzi adaptera przychodzą powiadomieniami - kawałkami, aż do znaku zachęty ">"
static void on_notify(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
    portENTER_CRITICAL(&mux);
    size_t n = std::min(len, RX_BUF_SIZE - 1 - rx_len);
    memcpy(rx_buf + rx_len, data, n);
    rx_len += n;
    rx_buf[rx_len] = 0;
    portEXIT_CRITICAL(&mux);
}

class ClientEvents : public BLEClientCallbacks {
    void onConnect(BLEClient *) override {}
    void onDisconnect(BLEClient *) override {
        tx_char = nullptr;
        state = BLE_OFF;
        set_text(status_text, sizeof(status_text), "Rozlaczono");
    }
};

static void ensure_init() {
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init("Diamel");
        Serial.printf("BLE: start, wolny RAM wewn.: %u KB\n",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
}

static void do_scan() {
    state = BLE_SCANNING;
    set_status("Szukanie adapterow...");
    BLEScan *scan = BLEDevice::getScan();
    scan->setActiveScan(true); // aktywne skanowanie - wiele adapterów podaje nazwę dopiero w odpowiedzi
    scan->setInterval(100);
    scan->setWindow(99);
    BLEScanResults *res = scan->start(SCAN_SECONDS, false);

    BleFound list[BLE_FOUND_MAX];
    int n = 0;
    for (int i = 0; res && i < res->getCount() && n < BLE_FOUND_MAX; i++) {
        BLEAdvertisedDevice d = res->getDevice(i);
        if (!d.haveName() || d.getName().length() == 0) continue; // bez nazwy - raczej nie adapter OBD
        strlcpy(list[n].name, d.getName().c_str(), sizeof(list[n].name));
        strlcpy(list[n].addr, d.getAddress().toString().c_str(), sizeof(list[n].addr));
        list[n].addr_type = d.getAddressType();
        list[n].rssi = (int8_t)d.getRSSI();
        n++;
    }
    scan->clearResults();
    std::sort(list, list + n, [](const BleFound &a, const BleFound &b) { return a.rssi > b.rssi; });

    portENTER_CRITICAL(&mux);
    memcpy(found, list, sizeof(BleFound) * n);
    found_count = n;
    portEXIT_CRITICAL(&mux);

    bool connected = client && client->isConnected();
    state = connected ? BLE_CONNECTED : BLE_OFF;
    char msg[64];
    snprintf(msg, sizeof(msg), "Znaleziono urzadzen: %d", n);
    set_status(msg);
}

static bool is_standard_service(const String &uuid) {
    // Usługi ogólne BLE (Generic Access/Attribute, Device Information, Battery) - nie przesyłają danych OBD
    return uuid.startsWith("00001800") || uuid.startsWith("00001801") || uuid.startsWith("0000180a") ||
           uuid.startsWith("0000180f");
}

// Adaptery różnych firm mają różne identyfikatory usług (np. FFF0, FFE0, własne 128-bit) - szukamy
// pierwszej usługi, która ma kanał z powiadomieniami (odbiór) i kanał z zapisem (nadawanie).
static bool find_channels() {
    std::map<std::string, BLERemoteService *> *services = client->getServices();
    if (!services) return false;
    for (auto &s : *services) {
        String uuid = s.second->getUUID().toString();
        uuid.toLowerCase();
        if (is_standard_service(uuid)) continue;
        std::map<std::string, BLERemoteCharacteristic *> *chars = s.second->getCharacteristics();
        if (!chars) continue;
        BLERemoteCharacteristic *rx = nullptr, *tx = nullptr;
        for (auto &c : *chars) {
            BLERemoteCharacteristic *ch = c.second;
            if (!rx && (ch->canNotify() || ch->canIndicate())) rx = ch;
            if (!tx && (ch->canWrite() || ch->canWriteNoResponse())) tx = ch;
        }
        if (rx && tx) {
            rx->registerForNotify(on_notify, rx->canNotify());
            tx_char = tx;
            tx_with_response = !tx->canWriteNoResponse();
            Serial.printf("BLE: usluga %s\n", uuid.c_str());
            return true;
        }
    }
    return false;
}

static void do_disconnect() {
    if (client && client->isConnected()) client->disconnect();
    tx_char = nullptr;
    state = BLE_OFF;
    set_status("Rozlaczono");
}

static void do_connect(const char *addr, uint8_t addr_type) {
    do_disconnect();
    state = BLE_CONNECTING;
    set_status("Laczenie...");
    if (!client) {
        client = BLEDevice::createClient();
        client->setClientCallbacks(new ClientEvents());
    }
    if (!client->connect(BLEAddress(String(addr), addr_type), addr_type, CONNECT_TIMEOUT_MS)) {
        state = BLE_OFF;
        set_status("Blad: adapter nie odpowiada (wylaczony, poza zasiegiem?)");
        return;
    }
    if (!find_channels()) {
        client->disconnect();
        state = BLE_OFF;
        set_status("Blad: to urzadzenie nie wyglada na adapter OBD");
        return;
    }
    state = BLE_CONNECTED;
    set_status("Polaczono");
}

bool ble_elm_command(const char *cmd, char *resp, size_t len, uint32_t timeout_ms) {
    if (!cmd_lock || !tx_char || !client || !client->isConnected()) return false;
    xSemaphoreTake(cmd_lock, portMAX_DELAY);
    portENTER_CRITICAL(&mux);
    rx_len = 0;
    rx_buf[0] = 0;
    portEXIT_CRITICAL(&mux);

    char line[64];
    size_t n = snprintf(line, sizeof(line), "%s\r", cmd);
    for (size_t off = 0; off < n && tx_char; off += WRITE_CHUNK) {
        tx_char->writeValue((uint8_t *)line + off, std::min((size_t)WRITE_CHUNK, n - off), tx_with_response);
    }

    bool done = false;
    uint32_t start = millis();
    while (!done && millis() - start < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        portENTER_CRITICAL(&mux);
        done = strchr(rx_buf, '>') != nullptr;
        portEXIT_CRITICAL(&mux);
    }

    // Odpowiedź bez znaku zachęty, nowe linie jako " | ", bez pustych linii
    char raw[RX_BUF_SIZE];
    portENTER_CRITICAL(&mux);
    memcpy(raw, rx_buf, rx_len + 1);
    portEXIT_CRITICAL(&mux);
    size_t o = 0;
    bool sep = false;
    for (char *p = raw; *p && *p != '>' && o + 4 < len; p++) {
        if (*p == '\r' || *p == '\n') {
            sep = o > 0;
            continue;
        }
        if (sep) {
            o += snprintf(resp + o, len - o, " | ");
            sep = false;
        }
        resp[o++] = *p;
    }
    resp[o] = 0;
    xSemaphoreGive(cmd_lock);
    return done;
}

static void do_test() {
    if (!tx_char) {
        set_text(test_text, sizeof(test_text), "Najpierw polacz z adapterem.");
        return;
    }
    state = BLE_TESTING;
    set_status("Test adaptera...");
    // Tylko komendy samego adaptera (AT) - nic nie jest wysyłane do sterowników auta
    static const struct { const char *cmd; const char *label; uint32_t timeout; } STEPS[] = {
        {"ATZ", "Reset", 3000}, {"ATE0", nullptr, 1000}, {"ATI", "Wersja", 1000},
        {"AT@1", "Opis", 1000}, {"ATRV", "Napiecie", 1000},
    };
    char out[200] = "", resp[64];
    size_t o = 0;
    bool any = false;
    for (auto &s : STEPS) {
        bool ok = ble_elm_command(s.cmd, resp, sizeof(resp), s.timeout);
        any |= ok;
        if (s.label) o += snprintf(out + o, sizeof(out) - o, "%s: %s\n", s.label, ok ? resp : "brak odpowiedzi");
    }
    set_text(test_text, sizeof(test_text), out);
    state = tx_char ? BLE_CONNECTED : BLE_OFF;
    set_status(any ? "Adapter odpowiada (ELM327)" : "Adapter nie odpowiada na komendy ELM327");
}

static void ble_task(void *) {
    Request r;
    for (;;) {
        if (xQueueReceive(queue, &r, portMAX_DELAY) != pdTRUE) continue;
        ensure_init();
        switch (r.type) {
            case REQ_SCAN:       do_scan(); break;
            case REQ_CONNECT:    do_connect(r.addr, r.addr_type); break;
            case REQ_DISCONNECT: do_disconnect(); break;
            case REQ_TEST:       do_test(); break;
        }
    }
}

static void post(ReqType type, const char *addr = "", uint8_t addr_type = 0) {
    if (!task) {
        queue = xQueueCreate(4, sizeof(Request));
        cmd_lock = xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(ble_task, "BLE", 8192, NULL, 2, &task, 0);
    }
    Request r = {type, "", addr_type};
    strlcpy(r.addr, addr, sizeof(r.addr));
    xQueueSend(queue, &r, 0);
}

void ble_scan() { post(REQ_SCAN); }
void ble_connect(const char *addr, uint8_t addr_type) { post(REQ_CONNECT, addr, addr_type); }
void ble_disconnect() { if (task) post(REQ_DISCONNECT); }
void ble_test() { post(REQ_TEST); }

BleState ble_state() { return state; }

int ble_found(BleFound *out, int max) {
    portENTER_CRITICAL(&mux);
    int n = std::min(found_count, max);
    memcpy(out, found, sizeof(BleFound) * n);
    portEXIT_CRITICAL(&mux);
    return n;
}

void ble_status_text(char *buf, size_t len) {
    portENTER_CRITICAL(&mux);
    strlcpy(buf, status_text, len);
    portEXIT_CRITICAL(&mux);
}

void ble_test_text(char *buf, size_t len) {
    portENTER_CRITICAL(&mux);
    strlcpy(buf, test_text, len);
    portEXIT_CRITICAL(&mux);
}
