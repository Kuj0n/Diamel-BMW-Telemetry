#include "backup.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include "catalog.h"
#include "clock.h"
#include "config.h"
#include "logger.h"

#define BACKUP_MAX_SIZE 16384

static void add_param(JsonArray arr, uint16_t id) {
    char key[64];
    param_name_key(id, key, sizeof(key));
    if (key[0]) arr.add(key);
    else arr.add(nullptr);
}

char *backup_to_json(size_t *len) {
    const Settings &s = g_settings;
    JsonDocument doc;
    char now[24];
    clock_format(now, sizeof(now));
    doc["aplikacja"] = "Diamel Telemetry";
    doc["wersja"] = FW_VERSION;
    doc["zapisano"] = now;

    JsonObject ekran = doc["ekran"].to<JsonObject>();
    ekran["jasnosc"] = s.brightness;
    ekran["start"] = s.start_screen;
    ekran["fps"] = s.show_fps;
    ekran["cpu"] = s.show_cpu;

    JsonArray strony = doc["strony"].to<JsonArray>();
    for (int p = 0; p < LIVE_PAGES; p++) {
        JsonArray page = strony.add<JsonArray>();
        for (int i = 0; i < s.page_count[p]; i++) add_param(page, s.page_ids[p][i]);
    }
    JsonArray wykres = doc["wykres"].to<JsonArray>();
    for (int i = 0; i < CHART_SERIES_MAX; i++) add_param(wykres, s.chart_series[i]);

    JsonObject log = doc["log"].to<JsonObject>();
    log["strona"] = s.log_page;
    log["interwal_ms"] = s.log_interval_ms;
    log["format"] = s.csv_format;

    JsonObject dane = doc["dane"].to<JsonObject>();
    dane["zrodlo"] = s.data_source;
    dane["autostart"] = s.comm_autostart;
    dane["odstep_ms"] = s.poll_interval_ms;
    dane["timeout_ms"] = s.poll_timeout_ms;
    dane["timeout_bledow_ms"] = s.dtc_timeout_ms;

    JsonObject sleep = doc["usypianie"].to<JsonObject>();
    sleep["wlaczone"] = s.sleep_enabled;
    char key[64];
    param_name_key(s.sleep_param, key, sizeof(key));
    sleep["parametr"] = key;
    sleep["wiekszy"] = s.sleep_above;
    sleep["prog"] = s.sleep_threshold;
    sleep["opoznienie_s"] = s.sleep_delay_s;
    sleep["brak_danych"] = s.sleep_on_missing;

    JsonObject zegar = doc["zegar"].to<JsonObject>();
    zegar["z_przegladarki"] = s.time_from_browser;
    zegar["rtc"] = s.rtc_installed;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["tryb"] = s.wifi_mode;
    wifi["haslo_wlasne"] = s.wifi_pass;
    wifi["siec_domowa"] = s.home_ssid;
    wifi["haslo_domowe"] = s.home_pass;

    JsonObject ble = doc["ble"].to<JsonObject>();
    ble["adres"] = s.ble_addr;
    ble["nazwa"] = s.ble_name;
    ble["typ"] = s.ble_addr_type;

    size_t n = measureJsonPretty(doc);
    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    serializeJsonPretty(doc, buf, n + 1);
    *len = n;
    return buf;
}

// Pole tylko jeśli jest w pliku i mieści się w zakresie
template <typename T>
static void take(JsonVariantConst v, T &field, long min, long max) {
    if (v.isNull()) return;
    long x = v.as<long>();
    if (x >= min && x <= max) field = (T)x;
}

static void take_str(JsonVariantConst v, char *field, size_t len) {
    if (v.is<const char *>()) strlcpy(field, v.as<const char *>(), len);
}

static uint16_t param_from(JsonVariantConst v, int &skipped) {
    if (!v.is<const char *>()) return PARAM_NONE;
    ParamIdx id = param_by_name_key(v.as<const char *>());
    if (id == PARAM_NONE) skipped++;
    return id;
}

bool backup_from_json(const char *json, size_t len, Settings &out, int &skipped, char *err, size_t err_len) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json, len);
    if (e) {
        snprintf(err, err_len, "Bledny plik JSON (%s)", e.c_str());
        return false;
    }
    if (strcmp(doc["aplikacja"] | "", "Diamel Telemetry") != 0) {
        snprintf(err, err_len, "To nie jest kopia ustawien Diamel Telemetry");
        return false;
    }
    out = g_settings; // czego nie ma w pliku - zostaje jak jest
    skipped = 0;

    take(doc["ekran"]["jasnosc"], out.brightness, 10, 255);
    take(doc["ekran"]["start"], out.start_screen, 0, START_COUNT - 1);
    take(doc["ekran"]["fps"], out.show_fps, 0, 1);
    take(doc["ekran"]["cpu"], out.show_cpu, 0, 1);

    JsonArrayConst strony = doc["strony"];
    for (int p = 0; p < LIVE_PAGES && p < (int)strony.size(); p++) {
        int n = 0;
        for (JsonVariantConst v : strony[p].as<JsonArrayConst>()) {
            uint16_t id = param_from(v, skipped);
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= out.page_ids[p][k] == id;
            if (id != PARAM_NONE && !dup && n < PAGE_MAX) out.page_ids[p][n++] = id;
        }
        out.page_count[p] = n;
    }
    JsonArrayConst wykres = doc["wykres"];
    for (int i = 0; i < CHART_SERIES_MAX && i < (int)wykres.size(); i++) {
        out.chart_series[i] = wykres[i].isNull() ? PARAM_NONE : param_from(wykres[i], skipped);
    }

    take(doc["log"]["strona"], out.log_page, 0, LOG_ALL_PAGES);
    take(doc["log"]["interwal_ms"], out.log_interval_ms, 50, 5000);
    take(doc["log"]["format"], out.csv_format, 0, CSV_COUNT - 1);

    take(doc["dane"]["zrodlo"], out.data_source, 0, SRC_COUNT - 1);
    take(doc["dane"]["autostart"], out.comm_autostart, 0, 1);
    take(doc["dane"]["odstep_ms"], out.poll_interval_ms, 10, 1000);
    take(doc["dane"]["timeout_ms"], out.poll_timeout_ms, 10, 1000);
    take(doc["dane"]["timeout_bledow_ms"], out.dtc_timeout_ms, 500, 10000);

    JsonObjectConst sl = doc["usypianie"];
    take(sl["wlaczone"], out.sleep_enabled, 0, 1);
    if (!sl["parametr"].isNull()) out.sleep_param = param_from(sl["parametr"], skipped);
    take(sl["wiekszy"], out.sleep_above, 0, 1);
    if (sl["prog"].is<float>()) out.sleep_threshold = sl["prog"].as<float>();
    take(sl["opoznienie_s"], out.sleep_delay_s, 10, 3600);
    take(sl["brak_danych"], out.sleep_on_missing, 0, 1);

    take(doc["zegar"]["z_przegladarki"], out.time_from_browser, 0, 1);
    take(doc["zegar"]["rtc"], out.rtc_installed, 0, 1);

    take(doc["wifi"]["tryb"], out.wifi_mode, 0, WIFI_MODE_COUNT - 1);
    const char *own = doc["wifi"]["haslo_wlasne"] | "";
    if (strlen(own) == WIFI_PASS_LEN) strlcpy(out.wifi_pass, own, sizeof(out.wifi_pass));
    take_str(doc["wifi"]["siec_domowa"], out.home_ssid, sizeof(out.home_ssid));
    take_str(doc["wifi"]["haslo_domowe"], out.home_pass, sizeof(out.home_pass));

    take_str(doc["ble"]["adres"], out.ble_addr, sizeof(out.ble_addr));
    take_str(doc["ble"]["nazwa"], out.ble_name, sizeof(out.ble_name));
    take(doc["ble"]["typ"], out.ble_addr_type, 0, 255);
    return true;
}

// Nazwa krótsza niż logi (limit 23 znaki): SET_RRRRMMDD_GGMM.json albo SET_0001.json
static void backup_name(char *name, size_t len) {
    struct tm tm;
    if (clock_now(tm)) {
        strftime(name, len, "SET_%Y%m%d_%H%M.json", &tm);
        return;
    }
    logger_new_name("SET", name, len); // SET_0001.csv -> .json
    char *dot = strrchr(name, '.');
    if (dot) strlcpy(dot, ".json", len - (dot - name));
}

bool backup_save_sd(char *name, size_t len) {
    if (!logger_sd_ok()) return false;
    size_t n;
    char *json = backup_to_json(&n);
    if (!json) return false;
    backup_name(name, len);
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
    File f = SD.open(path, FILE_WRITE);
    bool ok = f && f.write((const uint8_t *)json, n) == n;
    if (f) f.close();
    free(json);
    return ok;
}

bool backup_restore_sd(char *name, size_t name_len, char *err, size_t err_len) {
    name[0] = 0;
    if (!logger_sd_ok()) {
        snprintf(err, err_len, "Brak karty SD");
        return false;
    }
    // Najnowsza kopia = największa nazwa (data w nazwie)
    File dir = SD.open(LOG_DIR);
    for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
        const char *n = f.name();
        size_t l = strlen(n);
        if (!f.isDirectory() && !strncmp(n, "SET_", 4) && l > 5 && !strcmp(n + l - 5, ".json") && strcmp(n, name) > 0) {
            strlcpy(name, n, name_len);
        }
        f.close();
    }
    if (dir) dir.close();
    if (!name[0]) {
        snprintf(err, err_len, "Brak kopii SET_*.json na karcie");
        return false;
    }
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
    File f = SD.open(path);
    size_t size = f ? f.size() : 0;
    if (!f || size == 0 || size > BACKUP_MAX_SIZE) {
        snprintf(err, err_len, "Nie mozna odczytac %s", name);
        return false;
    }
    char *buf = (char *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    bool ok = buf && f.read((uint8_t *)buf, size) == size;
    f.close();
    Settings tmp;
    int skipped = 0;
    ok = ok && backup_from_json(buf, size, tmp, skipped, err, err_len);
    free(buf);
    if (!ok) return false;
    g_settings = tmp;
    settings_save();
    if (skipped) snprintf(err, err_len, "Pominieto parametrow spoza katalogu: %d", skipped);
    else err[0] = 0;
    return true;
}
