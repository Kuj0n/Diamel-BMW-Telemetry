#include "logger.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "config.h"
#include "settings.h"
#include "telemetry.h"

#define SD_SPI_HZ 20000000
#define FLUSH_EVERY_MS 1000
#define MAX_COLS (LIVE_MAX + CHART_SERIES_MAX)

static bool sd_ok = false;
static volatile bool want = false;   // żądanie z UI (rdzeń 1)
static volatile bool active = false; // stan zadania zapisu (rdzeń 0)
static volatile bool write_error = false;
static volatile uint32_t start_ms = 0;
static char current_name[24] = "";
static uint16_t next_number = 1;

// Kolejny numer pliku: brak zegara czasu rzeczywistego, więc LOG_0001.csv, LOG_0002.csv...
static void scan_next_number() {
    File dir = SD.open(LOG_DIR);
    if (!dir) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        int n;
        if (sscanf(f.name(), "LOG_%d.csv", &n) == 1 && n >= next_number) next_number = n + 1;
        f.close();
    }
    dir.close();
}

// Kolumny = wartości ze strony Odczyty + serie wykresu, bez powtórzeń
static int collect_columns(uint16_t *cols) {
    int n = 0;
    auto add = [&](uint16_t id) {
        if (id >= P_COUNT) return;
        for (int i = 0; i < n; i++) {
            if (cols[i] == id) return;
        }
        cols[n++] = id;
    };
    for (int i = 0; i < g_settings.live_count; i++) add(g_settings.live_ids[i]);
    for (int i = 0; i < CHART_SERIES_MAX; i++) add(g_settings.chart_series[i]);
    return n;
}

static File open_new_file(uint16_t *cols, int ncols) {
    char path[40];
    snprintf(current_name, sizeof(current_name), "LOG_%04u.csv", next_number++);
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, current_name);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return f;

    f.print("czas_s");
    for (int i = 0; i < ncols; i++) {
        const ParamInfo &p = PARAM_INFO[cols[i]];
        if (*p.unit) f.printf(",%s [%s]", p.name, p.unit);
        else f.printf(",%s", p.name);
    }
    f.print("\n");
    return f;
}

static void logger_task(void *) {
    File file;
    uint16_t cols[MAX_COLS];
    int ncols = 0;
    uint32_t last_flush = 0;
    char line[MAX_COLS * 14 + 16];
    TelemetrySnapshot s;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        if (want && !file) {
            ncols = collect_columns(cols);
            file = open_new_file(cols, ncols);
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
            telemetry_get(s);
            int pos = snprintf(line, sizeof(line), "%.2f", (millis() - start_ms) / 1000.0f);
            for (int i = 0; i < ncols && pos < (int)sizeof(line) - 16; i++) {
                uint16_t id = cols[i];
                if (s.valid[id]) pos += snprintf(line + pos, sizeof(line) - pos, ",%.*f", PARAM_INFO[id].decimals, s.value[id]);
                else line[pos++] = ',';
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

bool logger_take_error() {
    if (!write_error) return false;
    write_error = false;
    return true;
}
