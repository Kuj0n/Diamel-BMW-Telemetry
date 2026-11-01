#include "dtc.h"
#include <Arduino.h>
#include <SD.h>
#include "clock.h"
#include "comm.h"
#include "logger.h"
#include "settings.h"

#define DEMO_PER_MODULE 2
#define DTC_STATUS_MASK 0xFF // wszystkie kody, które mają jakikolwiek bit statusu

static volatile DtcState state = DTC_IDLE;
static DtcEntry *entries; // w PSRAM
static volatile int count;
static volatile bool simulated;
static int req_module;
static char status[120] = "Nacisnij \"Odczytaj\".";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char read_time[24]; // kiedy odczytano (do eksportu)

static void set_status(const char *fmt, ...) {
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&mux);
    strlcpy(status, buf, sizeof(status));
    portEXIT_CRITICAL(&mux);
}

static int targets(int *out, int max) {
    int n = 0;
    if (req_module > 0) {
        out[n++] = req_module;
        return n;
    }
    for (int m = 1; m < modules_count() && n < max; m++) {
        if (module_def(m).dtc_count) out[n++] = m;
    }
    return n;
}

static void add_entries(int module, const uint32_t *codes, const uint8_t *st, int k) {
    char (*texts)[DTC_TEXT_LEN] = (char (*)[DTC_TEXT_LEN])heap_caps_malloc(k * DTC_TEXT_LEN, MALLOC_CAP_SPIRAM);
    if (texts) catalog_dtc_texts(module_def(module).addr, codes, k, texts);
    for (int i = 0; i < k && count < DTC_MAX; i++) {
        DtcEntry &e = entries[count];
        e.module = module;
        e.code = codes[i];
        e.status = st[i];
        strlcpy(e.text, texts && texts[i][0] ? texts[i] : "(brak opisu w katalogu)", DTC_TEXT_LEN);
        count++;
    }
    free(texts);
}

// Tryb DEMO: kilka kodów z plików katalogu, z prawdziwymi opisami
static void read_demo() {
    int t[8], nt = 0;
    if (req_module > 0) {
        t[nt++] = req_module;
    } else {
        static const uint8_t ADDRS[] = {0x12, 0x19, 0x78}; // silnik, xDrive, klimatyzacja
        for (uint8_t a : ADDRS) {
            int m = module_by_addr(a);
            if (m > 0 && module_def(m).dtc_count) t[nt++] = m;
        }
    }
    for (int i = 0; i < nt; i++) {
        set_status("Odczyt: %s (%d/%d)...", module_def(t[i]).name, i + 1, nt);
        vTaskDelay(pdMS_TO_TICKS(400));
        uint32_t codes[DEMO_PER_MODULE];
        uint8_t st[DEMO_PER_MODULE] = {0x09, 0x08}; // aktywny, zapisany
        int k = catalog_dtc_samples(module_def(t[i]).addr, codes, DEMO_PER_MODULE);
        add_entries(t[i], codes, st, k);
    }
    simulated = true;
    set_status("DEMO: przykladowe kody z katalogu (symulacja), modulow: %d", nt);
    state = DTC_DONE;
}

// Auto: 19 02 maska -> 59 02 maska_dostępności [kod 3 bajty, status]...
static void read_car() {
    int t[64];
    int nt = targets(t, 64);
    static uint8_t resp[1024];
    int replied = 0;
    char silent[60] = "";
    for (int i = 0; i < nt; i++) {
        const ModuleDef &md = module_def(t[i]);
        set_status("Odczyt: %s (%d/%d)...", md.name, i + 1, nt);
        uint8_t req[3] = {0x19, 0x02, DTC_STATUS_MASK};
        size_t len;
        UdsResult r = uds_request(md.addr, req, 3, resp, sizeof(resp), &len, g_settings.dtc_timeout_ms);
        if (r == UDS_NO_BUS || r == UDS_NO_TRANSPORT) {
            // Pierwszy moduł bez magistrali = cała magistrala niedostępna - nie czekamy na resztę
            set_status("BLAD: %s", uds_result_text(r));
            state = DTC_FAILED;
            return;
        }
        if (r != UDS_OK || len < 3 || resp[0] != 0x59) {
            size_t o = strlen(silent);
            if (o < sizeof(silent) - 4) snprintf(silent + o, sizeof(silent) - o, "%s%02X", o ? " " : "", md.addr);
            continue;
        }
        replied++;
        int k = (len - 3) / 4;
        uint32_t codes[64];
        uint8_t st[64];
        int n = 0;
        for (int j = 0; j < k && n < 64; j++) {
            const uint8_t *rec = resp + 3 + j * 4;
            codes[n] = ((uint32_t)rec[0] << 16) | (rec[1] << 8) | rec[2];
            st[n++] = rec[3];
        }
        add_entries(t[i], codes, st, n);
    }
    if (!replied) {
        set_status("BLAD: zaden sterownik nie odpowiada (timeout) - zaplon wlaczony?");
        state = DTC_FAILED;
        return;
    }
    if (silent[0]) set_status("Odczytano z %d modulow. Bez odpowiedzi: %s", replied, silent);
    else set_status("Odczytano z %d modulow", replied);
    state = DTC_DONE;
}

static void dtc_task(void *) {
    clock_format(read_time, sizeof(read_time));
    if (g_settings.data_source == SRC_DEMO) read_demo();
    else read_car();
    vTaskDelete(NULL);
}

void dtc_read(int module) {
    if (state == DTC_READING) return;
    if (!entries) entries = (DtcEntry *)heap_caps_malloc(sizeof(DtcEntry) * DTC_MAX, MALLOC_CAP_SPIRAM);
    if (!entries) return;
    req_module = module;
    count = 0;
    simulated = false;
    state = DTC_READING;
    set_status("Odczyt...");
    // Rdzeń 0, duży stos - czytanie pliku opisów z karty
    xTaskCreatePinnedToCore(dtc_task, "DTC", 10240, NULL, 1, NULL, 0);
}

DtcState dtc_state() { return state; }

void dtc_status_text(char *buf, size_t len) {
    portENTER_CRITICAL(&mux);
    strlcpy(buf, status, len);
    portEXIT_CRITICAL(&mux);
}

int dtc_count() { return state == DTC_DONE ? count : 0; }
const DtcEntry &dtc_entry(int i) { return entries[i]; }
bool dtc_simulated() { return simulated; }

bool dtc_export(char *name, size_t len) {
    if (state != DTC_DONE || !logger_sd_ok()) return false;
    char path[48];
    logger_new_name("DTC", name, len);
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    char sep = g_settings.csv_format == CSV_EXCEL_PL ? ';' : ',';
    f.print("\xEF\xBB\xBF"); // BOM UTF-8 dla Excela
    f.printf("Odczyt bledow%c%s%c%s\n", sep, read_time, sep, simulated ? "DEMO (symulacja)" : "auto");
    f.printf("modul%cadres%ckod%cstatus%cstan%copis\n", sep, sep, sep, sep, sep);
    for (int i = 0; i < count; i++) {
        const DtcEntry &e = entries[i];
        const ModuleDef &md = module_def(e.module);
        f.printf("\"%s\"%c%02X%c%06lX%c%02X%c%s%c\"%s\"\n", md.name, sep, md.addr, sep, (unsigned long)e.code, sep,
                 e.status, sep, (e.status & 0x01) ? "aktywny" : "zapisany", sep, e.text);
    }
    if (!count) f.print("Brak zapisanych bledow\n");
    f.close();
    return true;
}
