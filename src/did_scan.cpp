#include "did_scan.h"
#include <Arduino.h>
#include <SD.h>
#include <algorithm>
#include "catalog.h"
#include "comm.h"
#include "logger.h"

#define SCAN_TIMEOUT_MS 300
#define SCAN_GAP_MS 20 // przerwa między zapytaniami (sterownik nie lubi zapytań bez przerwy)
#define SILENT_CHECK 20 // tyle braków odpowiedzi z rzędu -> sprawdzamy, czy sterownik w ogóle żyje

static DidScanState st;
static volatile bool stop_req = false;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static const char *result_name(UdsResult r) {
    switch (r) {
        case UDS_OK:           return "OK";
        case UDS_NEGATIVE:     return "odmowa";
        case UDS_NO_REPLY:     return "cisza";
        case UDS_NO_BUS:       return "brak_magistrali";
        case UDS_BLOCKED:      return "zablokowane";
        case UDS_NO_TRANSPORT: return "brak_adaptera";
    }
    return "?";
}

static void set_note(const char *text) {
    portENTER_CRITICAL(&mux);
    strlcpy(st.note, text, sizeof(st.note));
    portEXIT_CRITICAL(&mux);
}

// Numer VIN (22 F190) - odpowiada każdy sterownik przy włączonym zapłonie
static bool ecu_alive(uint8_t *resp, size_t cap) {
    const uint8_t vin[3] = {0x22, 0xF1, 0x90};
    size_t len = 0;
    return uds_request(st.addr, vin, 3, resp, cap, &len, 1000) == UDS_OK;
}

static bool block_mode = false;

static const char *did_name(const ModuleDef &md, uint16_t did) {
    for (int i = md.first; i < md.first + md.count; i++) {
        if (param(i).did == did) return param(i).name;
    }
    return "";
}

// Tryb zestawu: grupy po 20 DID; odrzucona grupa dzielona na pół (stos przedziałów)
static void block_scan(File &f, const ModuleDef &md, const uint16_t *dids, int n) {
    uint8_t *sizes = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!sizes) return;
    for (int k = 0; k < n; k++) { // rozmiar elementu = do końca najdalszej wartości z tego DID
        int sz = 1;
        for (int i = md.first; i < md.first + md.count; i++) {
            if (param(i).did != dids[k]) continue;
            static const uint8_t SIZES[] = {1, 1, 2, 2, 4, 4, 4};
            sz = std::max<int>(sz, param(i).pos + SIZES[param(i).type]);
        }
        sizes[k] = (uint8_t)std::min(sz, 32);
    }
    struct Range {
        int from, len;
    } stack[64];
    int sp = 0, next = 0; // next = początek kolejnej grupy 20 DID
    static uint8_t resp[700];
    int silent_run = 0;
    while ((sp || next < n) && !stop_req) {
        if (!sp) {
            stack[sp++] = {next, std::min(20, n - next)};
            next += 20;
        }
        Range r = stack[--sp];
        size_t len = 0;
        UdsResult u = uds_block_probe(st.addr, dids + r.from, sizes + r.from, r.len, resp, sizeof(resp), &len);
        if (u == UDS_NO_BUS) {
            set_note("brak magistrali CAN");
            break;
        }
        bool ok = u == UDS_OK && len >= 3 && resp[0] == 0x62;
        if (!ok && u == UDS_NEGATIVE && r.len > 1 && sp < 62) { // odrzucony zestaw: połówki
            int h = r.len / 2;
            stack[sp++] = {r.from + h, r.len - h};
            stack[sp++] = {r.from, h};
            continue;
        }
        size_t o = 3;
        for (int k = r.from; k < r.from + r.len; k++) {
            const char *res = ok ? "OK" : result_name(u);
            if (f) {
                f.printf("%04X;%s;", dids[k], res);
                if (ok) {
                    for (int b = 0; b < sizes[k] && o + b < len; b++) f.printf("%02X ", resp[o + b]);
                } else if (u == UDS_NEGATIVE && len >= 3) {
                    f.printf("7F %02X %02X", resp[1], resp[2]);
                }
                f.printf(";%s\n", did_name(md, dids[k]));
            }
            o += sizes[k];
            portENTER_CRITICAL(&mux);
            st.done++;
            if (ok) st.ok++;
            else if (u == UDS_NEGATIVE) st.refused++;
            else st.silent++;
            portEXIT_CRITICAL(&mux);
        }
        silent_run = u == UDS_NO_REPLY ? silent_run + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(SCAN_GAP_MS));
        if (silent_run >= 3 && !ecu_alive(resp, sizeof(resp))) {
            set_note("sterownik przestal odpowiadac (takze na VIN) - zaplon?");
            break;
        }
    }
    free(sizes);
}

static void scan_task(void *) {
    int m = module_by_addr(st.addr);
    const ModuleDef &md = module_def(m);
    // Różne DID modułu (kilka parametrów bywa w jednym DID); mapa bitowa 64 Kb w PSRAM
    uint8_t *seen = (uint8_t *)heap_caps_calloc(8192, 1, MALLOC_CAP_SPIRAM);
    uint16_t *dids = (uint16_t *)heap_caps_malloc(md.count * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    for (int i = md.first; seen && dids && i < md.first + md.count; i++) {
        uint16_t d = param(i).did;
        if (seen[d >> 3] & (1 << (d & 7))) continue;
        seen[d >> 3] |= 1 << (d & 7);
        dids[n++] = d;
    }
    free(seen);
    portENTER_CRITICAL(&mux);
    st.total = n;
    portEXIT_CRITICAL(&mux);

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, st.file);
    File f = SD.open(path, FILE_WRITE);
    if (f) f.print("did;wynik;bajty;nazwa\n");
    static uint8_t resp[128];
    bool alive = ecu_alive(resp, sizeof(resp));
    if (f) f.printf("F190;%s;;Numer VIN (kontrola na starcie)\n", alive ? "OK" : "cisza");
    if (!alive) set_note("sterownik nie odpowiada na VIN - zaplon wlaczony?");
    if (alive && block_mode) block_scan(f, md, dids, n);
    int silent_run = 0;
    for (int k = 0; alive && !block_mode && k < n && !stop_req; k++) {
        const uint8_t req[3] = {0x22, (uint8_t)(dids[k] >> 8), (uint8_t)dids[k]};
        size_t len = 0;
        UdsResult r = uds_request(st.addr, req, 3, resp, sizeof(resp), &len, SCAN_TIMEOUT_MS);
        if (r == UDS_NO_BUS) {
            set_note("brak magistrali CAN");
            break;
        }
        const char *res = result_name(r);
        if (f) {
            // Nazwa pierwszego parametru z tym DID - żeby od razu było wiadomo, co działa
            const char *name = "";
            for (int i = md.first; i < md.first + md.count; i++) {
                if (param(i).did == dids[k]) { name = param(i).name; break; }
            }
            f.printf("%04X;%s;", dids[k], res);
            for (size_t b = 0; b < len && b < 24; b++) f.printf("%02X ", resp[b]);
            if (r == UDS_NO_REPLY) {
                // Co przyszło z magistrali zamiast odpowiedzi - do diagnozy
                UdsRxDebug d;
                uds_last_rx(d);
                f.printf("ramek %u", d.frames);
                if (d.frames) {
                    f.printf(", ostatnia %03X:", (unsigned)d.last_id);
                    for (int b = 0; b < 8; b++) f.printf(" %02X", d.last[b]);
                }
            }
            f.printf(";%s\n", name);
        }
        portENTER_CRITICAL(&mux);
        st.done = k + 1;
        if (r == UDS_OK) st.ok++;
        else if (r == UDS_NEGATIVE) st.refused++;
        else st.silent++;
        portEXIT_CRITICAL(&mux);
        // Długa seria ciszy: jeśli sterownik nie podaje nawet VIN, przerywamy (np. zapłon się wyłączył)
        silent_run = r == UDS_NO_REPLY ? silent_run + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(SCAN_GAP_MS));
        if (silent_run >= SILENT_CHECK) {
            silent_run = 0;
            if (!ecu_alive(resp, sizeof(resp))) {
                if (f) f.printf("F190;cisza;;Numer VIN - sterownik przestal odpowiadac, skan przerwany\n");
                set_note("sterownik przestal odpowiadac (takze na VIN) - zaplon?");
                break;
            }
        }
    }
    if (f) f.close();
    free(dids);
    uds_release();
    Serial.printf("Skan %02X: %d/%d, OK %d, odmowa %d, cisza %d -> %s\n", st.addr, st.done, st.total, st.ok,
                  st.refused, st.silent, path);
    st.running = false;
    vTaskDelete(NULL);
}

bool did_scan_start(uint8_t addr, bool block) {
    if (st.running || comm_running() || !logger_sd_ok() || module_by_addr(addr) < 0) return false;
    memset(&st, 0, sizeof(st));
    st.addr = addr;
    char prefix[12];
    snprintf(prefix, sizeof(prefix), "%c%02X", block ? 'B' : 'S', addr); // np. B12_20261101_223045.csv
    block_mode = block;
    logger_new_name(prefix, st.file, sizeof(st.file));
    stop_req = false;
    st.running = true;
    if (xTaskCreatePinnedToCore(scan_task, "SCAN", 6144, NULL, 2, NULL, 0) != pdPASS) {
        st.running = false;
        return false;
    }
    return true;
}

void did_scan_stop() { stop_req = true; }

void did_scan_state(DidScanState &out) {
    portENTER_CRITICAL(&mux);
    out = st;
    portEXIT_CRITICAL(&mux);
}
