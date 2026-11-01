#include "logger.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <sd_diskio.h>
#include <ff.h>
#include "catalog.h"
#include "clock.h"
#include "config.h"
#include "settings.h"
#include "telemetry.h"

#define SD_SPI_HZ 20000000
#define FLUSH_EVERY_MS 1000
#define MAX_COLS (LIVE_MAX + CHART_SERIES_MAX)
#define FORMAT_CLUSTER 32768     // duże klastry: szybsze formatowanie i dopisywanie do logów
#define FORMAT_WORK_BUF 16384    // bufor roboczy f_mkfs (większy = szybciej)

static volatile bool sd_ok = false;
static volatile bool formatting = false;
static volatile bool format_ok = false;
static volatile bool want = false;   // żądanie z UI (rdzeń 1)
static volatile bool active = false; // stan zadania zapisu (rdzeń 0)
static volatile bool write_error = false;
static volatile uint32_t start_ms = 0;
static char current_name[24] = "";
static uint16_t next_number = 1;

// Numer dla nazw bez daty (zegar nieustawiony): LOG_0001.csv, DTC_0002.csv... - wspólna numeracja
static void scan_next_number() {
    File dir = SD.open(LOG_DIR);
    if (!dir) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        int n;
        const char *name = f.name();
        if (strlen(name) == 12 && sscanf(name + 4, "%d.csv", &n) == 1 && n >= next_number) next_number = n + 1;
        f.close();
    }
    dir.close();
}

void logger_new_name(const char *prefix, char *name, size_t len) {
    struct tm tm;
    if (clock_now(tm)) {
        char stamp[20];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        snprintf(name, len, "%.3s_%s.csv", prefix, stamp); // np. LOG_20261101_153245.csv (23 znaki)
    } else {
        snprintf(name, len, "%.3s_%04u.csv", prefix, next_number++);
    }
}

// Kolumny = wartości ze strony Odczyty + serie wykresu, bez powtórzeń
static int collect_columns(uint16_t *cols) {
    int n = 0;
    auto add = [&](uint16_t id) {
        if (id >= params_count()) return;
        for (int i = 0; i < n; i++) {
            if (cols[i] == id) return;
        }
        cols[n++] = id;
    };
    for (int i = 0; i < g_settings.live_count; i++) add(g_settings.live_ids[i]);
    for (int i = 0; i < CHART_SERIES_MAX; i++) add(g_settings.chart_series[i]);
    return n;
}

static File open_new_file(uint16_t *cols, int ncols, char sep) {
    char path[40];
    logger_new_name("LOG", current_name, sizeof(current_name));
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, current_name);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return f;

    // BOM UTF-8 - Excel poprawnie pokaże "°C"
    f.print("\xEF\xBB\xBF" "czas_s");
    for (int i = 0; i < ncols; i++) {
        // W cudzysłowie - nazwy z katalogu mogą zawierać przecinki
        const ParamDef &p = param(cols[i]);
        if (*p.unit) f.printf("%c\"%s [%s]\"", sep, p.name, p.unit);
        else f.printf("%c\"%s\"", sep, p.name);
    }
    f.print("\n");
    return f;
}

// Liczba do bufora; przy przecinku dziesiętnym (Excel PL) kropka zamieniana na przecinek.
// Zwraca liczbę znaków albo -1, gdy brak miejsca.
static int put_number(char *buf, int space, float v, int decimals, char dec) {
    int n = snprintf(buf, space, "%.*f", decimals, v);
    if (n < 0 || n >= space) return -1;
    if (dec != '.') {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '.') buf[i] = dec;
        }
    }
    return n;
}

static void logger_task(void *) {
    File file;
    uint16_t cols[MAX_COLS];
    int ncols = 0;
    uint32_t last_flush = 0;
    char line[MAX_COLS * 16 + 32];
    char sep = ',', dec = '.';
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        if (want && !file) {
            ncols = collect_columns(cols);
            bool excel = g_settings.csv_format == CSV_EXCEL_PL;
            sep = excel ? ';' : ',';
            dec = excel ? ',' : '.';
            file = open_new_file(cols, ncols, sep);
            if (file) {
                start_ms = millis();
                last_flush = start_ms;
                active = true;
                Serial.printf("Log: start %s (%d kolumn)\n", current_name, ncols);
            } else {
                Serial.println("Log: BLAD otwarcia pliku");
                want = false;
                write_error = true;
            }
        } else if (!want && file) {
            file.close();
            active = false;
            Serial.printf("Log: stop %s\n", current_name);
            current_name[0] = 0;
        }

        if (file) {
            // Ostatni bajt bufora zostaje na znak nowej linii
            const int space = sizeof(line) - 1;
            int pos = put_number(line, space, (millis() - start_ms) / 1000.0f, 2, dec);
            if (pos < 0) pos = 0;
            for (int i = 0; i < ncols && pos < space; i++) {
                uint16_t id = cols[i];
                line[pos++] = sep;
                float v;
                if (!telemetry_value(id, v)) continue;
                int n = put_number(line + pos, space - pos, v, param(id).decimals, dec);
                if (n < 0) break; // nie zmieści się - reszta wiersza pusta
                pos += n;
            }
            line[pos++] = '\n';

            if (file.write((const uint8_t *)line, pos) != (size_t)pos) {
                // Np. wyjęta karta - kończymy nagrywanie, UI pokaże komunikat
                Serial.println("Log: BLAD zapisu");
                file.close();
                want = false;
                active = false;
                write_error = true;
                current_name[0] = 0;
            } else if (millis() - last_flush > FLUSH_EVERY_MS) {
                file.flush();
                last_flush = millis();
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(g_settings.log_interval_ms));
    }
}

void logger_init() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sd_ok = SD.begin(SD_CS, SPI, SD_SPI_HZ);
    if (sd_ok) {
        if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
        scan_next_number();
        Serial.printf("Karta SD: OK, %llu MB, nastepny log: LOG_%04u.csv\n",
                      SD.cardSize() / (1024 * 1024), next_number);
    } else {
        Serial.println("Karta SD: BRAK lub blad odczytu");
    }
    xTaskCreatePinnedToCore(logger_task, "LOG", 8192, NULL, 1, NULL, 0);
}

bool logger_sd_ok() { return sd_ok; }

bool logger_start() {
    if (!sd_ok) return false;
    want = true;
    return true;
}

void logger_stop() { want = false; }

bool logger_active() { return active; }

uint32_t logger_elapsed_ms() { return active ? millis() - start_ms : 0; }

const char *logger_current_file() { return current_name; }

bool logger_valid_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 23 || name[0] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

bool logger_delete(const char *name) {
    if (!sd_ok || !logger_valid_name(name)) return false;
    if (strcmp(name, current_name) == 0) return false; // plik w trakcie zapisu
    char path[40];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
    return SD.remove(path);
}

// Biblioteka SD nie ma formatowania: odmontowanie, ponowne podpięcie samego sterownika karty,
// f_mkfs (tak jak robi to biblioteka przy pustej karcie) i normalne zamontowanie.
// Katalog parametrów (/diamel) przeżywa formatowanie: kopia w PSRAM przed, zapis z powrotem po
struct CatalogFile {
    char name[24];
    uint8_t *data;
    size_t size;
};
#define CATALOG_FILES_MAX 64
static CatalogFile *backup;
static int backup_count;
static char format_error[64];

static void backup_free() {
    for (int i = 0; i < backup_count; i++) heap_caps_free(backup[i].data);
    heap_caps_free(backup);
    backup = nullptr;
    backup_count = 0;
}

static bool backup_catalog() {
    backup_count = 0;
    File dir = SD.open(CATALOG_DIR);
    if (!dir) return true; // nie ma czego chronić
    backup = (CatalogFile *)heap_caps_calloc(CATALOG_FILES_MAX, sizeof(CatalogFile), MALLOC_CAP_SPIRAM);
    bool ok = backup != nullptr;
    for (File f = dir.openNextFile(); ok && f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            CatalogFile &c = backup[backup_count];
            if (backup_count >= CATALOG_FILES_MAX) ok = false;
            else {
                strlcpy(c.name, f.name(), sizeof(c.name));
                c.size = f.size();
                c.data = (uint8_t *)heap_caps_malloc(c.size ? c.size : 1, MALLOC_CAP_SPIRAM);
                ok = c.data && f.read(c.data, c.size) == c.size;
                if (c.data) backup_count++;
            }
        }
        f.close();
    }
    dir.close();
    if (!ok) backup_free();
    return ok;
}

static bool restore_catalog() {
    if (!backup) return true;
    SD.mkdir(CATALOG_DIR);
    bool ok = true;
    for (int i = 0; i < backup_count; i++) {
        char path[48];
        snprintf(path, sizeof(path), "%s/%s", CATALOG_DIR, backup[i].name);
        File f = SD.open(path, FILE_WRITE);
        ok &= f && f.write(backup[i].data, backup[i].size) == backup[i].size;
        if (f) f.close();
    }
    Serial.printf("Karta SD: katalog przywrocony (%d plikow)%s\n", backup_count, ok ? "" : " - BLAD zapisu");
    backup_free();
    return ok;
}

static void format_task(void *) {
    bool ok = false;
    format_error[0] = 0;
    if (!backup_catalog()) {
        // Bez kopii katalogu nie formatujemy - baza parametrów i błędów by przepadła
        strlcpy(format_error, "Za malo pamieci na kopie katalogu /diamel", sizeof(format_error));
        format_ok = false;
        sd_ok = true;
        formatting = false;
        vTaskDelete(NULL);
        return;
    }
    SD.end();
    uint8_t pdrv = sdcard_init(SD_CS, &SPI, SD_SPI_HZ);
    if (pdrv != 0xFF) {
        size_t len = FORMAT_WORK_BUF;
        void *work = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!work) {
            len = FF_MAX_SS;
            work = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (work) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            const MKFS_PARM opt = {FM_ANY, 0, 0, 0, FORMAT_CLUSTER};
            uint32_t t0 = millis();
            FRESULT res = f_mkfs(drv, &opt, work, len);
            ok = res == FR_OK;
            Serial.printf("Karta SD: formatowanie %s (kod %d, %u ms)\n", ok ? "OK" : "BLAD", (int)res,
                          (unsigned)(millis() - t0));
            heap_caps_free(work);
        }
        sdcard_uninit(pdrv);
    }

    bool mounted = SD.begin(SD_CS, SPI, SD_SPI_HZ);
    if (mounted) {
        SD.mkdir(LOG_DIR);
        next_number = 1;
        if (!restore_catalog()) strlcpy(format_error, "Blad przywracania katalogu /diamel", sizeof(format_error));
    } else {
        backup_free();
    }
    sd_ok = mounted;
    format_ok = ok && mounted;
    formatting = false;
    vTaskDelete(NULL);
}

bool logger_format_start() {
    // Ekran (rdzeń 1) i serwer WWW (rdzeń 0) mogą wywołać jednocześnie
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&lock);
    bool can = !want && !active && !formatting;
    if (can) formatting = true;
    portEXIT_CRITICAL(&lock);
    if (!can) return false;

    sd_ok = false; // od teraz UI i serwer WWW traktują kartę jak wyjętą
    xTaskCreatePinnedToCore(format_task, "SD_FMT", 6144, NULL, 1, NULL, 0);
    return true;
}

bool logger_formatting() { return formatting; }

bool logger_format_ok() { return format_ok; }

const char *logger_format_error() { return format_error; }

bool logger_take_error() {
    if (!write_error) return false;
    write_error = false;
    return true;
}
