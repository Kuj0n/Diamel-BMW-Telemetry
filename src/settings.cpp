#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>

// Zmiana układu istniejących pól = podbij wersję (stare ustawienia zostaną zastąpione domyślnymi)
#define SETTINGS_VERSION 2
#define SAVE_DELAY_MS 1500
#define REFS_MAGIC 0xA5

Settings g_settings;
static bool dirty = false;
static uint32_t dirty_ms = 0;

// Bez znaków mylących się na ekranie (0/O, 1/l/I)
static void random_password(char *out) {
    static const char CHARS[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < WIFI_PASS_LEN; i++) out[i] = CHARS[esp_random() % (sizeof(CHARS) - 1)];
    out[WIFI_PASS_LEN] = 0;
}

static bool password_ok(const char *p) {
    for (int i = 0; i < WIFI_PASS_LEN; i++) {
        if (!isalnum((unsigned char)p[i])) return false;
    }
    return p[WIFI_PASS_LEN] == 0;
}

static void set_defaults(Settings &s) {
    memset(&s, 0, sizeof(s));
    s.version = SETTINGS_VERSION;
    s.brightness = 255;
    s.start_screen = START_MENU;
    static const uint16_t default_live[] = {
        P_RPM, P_BATTERY, P_BOOST_TARGET, P_BOOST_ACTUAL, P_IAT, P_EGT,
        P_COOLANT, P_OIL_TEMP, P_DPF_SOOT, P_XDRIVE_CLUTCH, P_AC_PRESSURE, P_EVAP_TEMP};
    s.live_count = sizeof(default_live) / sizeof(default_live[0]);
    memcpy(s.live_ids, default_live, sizeof(default_live));
    s.chart_series[0] = P_BOOST_TARGET;
    s.chart_series[1] = P_BOOST_ACTUAL;
    s.chart_series[2] = PARAM_NONE;
    s.poll_interval_ms = 50;
    s.poll_timeout_ms = 50;
    s.dtc_timeout_ms = 3000;
    s.stop_no_reply_s = 5;
    s.stop_rpm_zero_min = 5;
    s.log_interval_ms = 200;
    s.csv_format = CSV_EXCEL_PL;
    s.wifi_pass[0] = 0; // wylosowane w settings_load()
    s.data_source = SRC_DEMO;
    s.show_fps = 0;
    s.show_cpu = 0;
    // Uśpienie: obroty < 1 przez 5 min (dłużej niż postój start-stop), domyślnie wyłączone
    s.sleep_param = P_RPM;
    s.sleep_ref = P_RPM;
    s.sleep_threshold = 1;
    s.sleep_delay_s = 300;
    s.sleep_enabled = 0;
    s.sleep_above = 0;
    s.sleep_on_missing = 1;
}

static bool is_valid(const Settings &s) {
    if (s.version != SETTINGS_VERSION || s.start_screen >= START_COUNT || s.live_count > LIVE_MAX ||
        s.csv_format >= CSV_COUNT) {
        return false;
    }
    // Wybrane parametry sprawdza settings_resolve_params() - zależą od wczytanego katalogu
    return true;
}

// Identyfikatory parametrów z katalogu nie dały się odnaleźć (np. brak karty) - nie nadpisujemy
// ich przy zapisie, żeby wybór wrócił, gdy katalog znów będzie dostępny
static bool refs_frozen = false;
static bool refs_resolved = false; // przed wczytaniem katalogu live_ids nie są jeszcze indeksami rejestru

static void refs_from_ids() {
    if (refs_frozen || !refs_resolved) return;
    g_settings.live_ref_count = g_settings.live_count;
    for (int i = 0; i < g_settings.live_count; i++) g_settings.live_refs[i] = param_ref(g_settings.live_ids[i]);
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        uint16_t id = g_settings.chart_series[i];
        g_settings.chart_refs[i] = id == PARAM_NONE ? PARAM_NONE : param_ref(id);
    }
    if (g_settings.sleep_param != PARAM_NONE) g_settings.sleep_ref = param_ref(g_settings.sleep_param);
    g_settings.refs_magic = REFS_MAGIC;
}

void settings_resolve_params() {
    Settings &s = g_settings;
    if (s.refs_magic != REFS_MAGIC) {
        // Zapis sprzed katalogu: live_ids to numery parametrów DEMO = ich identyfikatory
        s.live_ref_count = s.live_count;
        for (int i = 0; i < s.live_count; i++) s.live_refs[i] = s.live_ids[i];
        for (int i = 0; i < CHART_SERIES_MAX; i++) s.chart_refs[i] = s.chart_series[i];
        s.refs_magic = REFS_MAGIC;
    }
    int n = 0, lost = 0;
    for (int i = 0; i < s.live_ref_count && i < LIVE_MAX; i++) {
        ParamIdx id = param_by_ref(s.live_refs[i]);
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= s.live_ids[k] == id;
        if (id == PARAM_NONE) lost += ref_is_catalog(s.live_refs[i]);
        else if (!dup) s.live_ids[n++] = id;
    }
    s.live_count = n;
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        s.chart_series[i] = param_by_ref(s.chart_refs[i]);
        if (s.chart_series[i] == PARAM_NONE && s.chart_refs[i] != PARAM_NONE) lost += ref_is_catalog(s.chart_refs[i]);
    }
    s.sleep_param = param_by_ref(s.sleep_ref);
    if (s.sleep_param == PARAM_NONE) lost += ref_is_catalog(s.sleep_ref);
    refs_frozen = lost > 0;
    refs_resolved = true;
    if (lost) Serial.printf("Ustawienia: %d wybranych parametrow nie ma w katalogu (zachowane do czasu jego wczytania)\n", lost);
}

void settings_load() {
    Preferences prefs;
    prefs.begin("diamel", false);
    // Najpierw domyślne, potem nadpisanie zapisanym blokiem - krótszy (starszy) zapis
    // zostawia domyślne wartości w polach dopisanych później
    Settings tmp;
    set_defaults(tmp);
    size_t len = prefs.getBytesLength("cfg");
    bool ok = len > 0 && len <= sizeof(tmp) &&
              prefs.getBytes("cfg", &tmp, len) == len &&
              is_valid(tmp);
    if (ok && tmp.log_interval_ms < 50) tmp.log_interval_ms = 200;
    if (ok && tmp.data_source >= SRC_COUNT) tmp.data_source = SRC_DEMO;
    if (ok) {
        tmp.ble_addr[sizeof(tmp.ble_addr) - 1] = 0;
        tmp.ble_name[sizeof(tmp.ble_name) - 1] = 0;
    }
    prefs.end();

    if (ok) {
        g_settings = tmp;
        Serial.println("Ustawienia: wczytane z pamieci.");
    } else {
        set_defaults(g_settings);
        Serial.println("Ustawienia: domyslne.");
    }
    if (g_settings.brightness < 10) g_settings.brightness = 10;
    // Pierwszy start albo zapis sprzed dodania Wi-Fi - nowe hasło
    if (!password_ok(g_settings.wifi_pass)) settings_new_wifi_password();
}

void settings_save() {
    refs_from_ids();
    Preferences prefs;
    prefs.begin("diamel", false);
    prefs.putBytes("cfg", &g_settings, sizeof(g_settings));
    prefs.end();
    dirty = false;
    Serial.println("Ustawienia: zapisane.");
}

void settings_mark_dirty() {
    dirty = true;
    dirty_ms = millis();
}

void settings_tick() {
    if (dirty && millis() - dirty_ms > SAVE_DELAY_MS) settings_save();
}

void settings_flush() {
    if (dirty) settings_save();
}

const char *data_source_name(uint8_t source) {
    static const char *const NAMES[SRC_COUNT] = {"DEMO", "CAN", "BLE"};
    return source < SRC_COUNT ? NAMES[source] : "?";
}

void settings_new_wifi_password() {
    random_password(g_settings.wifi_pass);
    settings_save();
}
