#include "power.h"
#include <Arduino.h>
#include "catalog.h"
#include "display.h"
#include "logger.h"
#include "settings.h"
#include "telemetry.h"

static bool sleeping = false;
static uint32_t cond_since = 0; // 0 = warunek niespełniony

static void go_sleep() {
    sleeping = true;
    logger_stop(); // zamyka plik CSV - nic nie ginie przy wyłączeniu zasilania
    display_sleep(true);
    display_set_brightness(0);
    Serial.println("Uspienie: warunek spelniony, ekran wylaczony");
}

static void wake() {
    sleeping = false;
    cond_since = 0;
    display_sleep(false);
    display_set_brightness(g_settings.brightness);
    Serial.println("Uspienie: wybudzono dotykiem");
}

static bool condition_met() {
    ParamIdx id = g_settings.sleep_param;
    if (id == PARAM_NONE || id >= params_count()) return false;
    float v;
    if (!telemetry_value(id, v)) return g_settings.sleep_on_missing;
    return g_settings.sleep_above ? v > g_settings.sleep_threshold : v < g_settings.sleep_threshold;
}

void power_tick() {
    if (sleeping) {
        if (display_take_wake()) wake();
        return;
    }
    if (!g_settings.sleep_enabled || !condition_met()) {
        cond_since = 0;
        return;
    }
    if (!cond_since) cond_since = millis() | 1;
    if (millis() - cond_since >= (uint32_t)g_settings.sleep_delay_s * 1000) go_sleep();
}

bool power_sleeping() { return sleeping; }

int power_seconds_left() {
    if (sleeping) return 0;
    if (!cond_since) return -1;
    int32_t left = (int32_t)g_settings.sleep_delay_s - (int32_t)((millis() - cond_since) / 1000);
    return left > 0 ? left : 0;
}
