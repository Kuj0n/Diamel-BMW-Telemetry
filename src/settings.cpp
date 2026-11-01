#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>

// Zmiana układu istniejących pól = podbij wersję (stare ustawienia zostaną zastąpione domyślnymi)
#define SETTINGS_VERSION 2
#define SAVE_DELAY_MS 1500

Settings g_settings;
static bool dirty = false;
static uint32_t dirty_ms = 0;

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
}

static bool is_valid(const Settings &s) {
    if (s.version != SETTINGS_VERSION || s.start_screen >= START_COUNT || s.live_count > LIVE_MAX) return false;
    bool seen[P_COUNT] = {false};
    for (int i = 0; i < s.live_count; i++) {
        if (s.live_ids[i] >= P_COUNT || seen[s.live_ids[i]]) return false;
        seen[s.live_ids[i]] = true;
    }
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        if (s.chart_series[i] != PARAM_NONE && s.chart_series[i] >= P_COUNT) return false;
    }
    return true;
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
    prefs.end();

    if (ok) {
        g_settings = tmp;
        Serial.println("Ustawienia: wczytane z pamieci.");
    } else {
        set_defaults(g_settings);
        Serial.println("Ustawienia: domyslne.");
    }
    if (g_settings.brightness < 10) g_settings.brightness = 10;
}

void settings_save() {
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
