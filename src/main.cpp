#include <Arduino.h>
#include <lvgl.h>
#include "catalog.h"
#include "clock.h"
#include "config.h"
#include "display.h"
#include "logger.h"
#include "settings.h"
#include "telemetry.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- Diamel Telemetry: Start ---");
    Serial.printf("Wersja: %s\n", FW_VERSION);
    Serial.printf("PSRAM: %s, rozmiar: %u KB\n", psramFound() ? "TAK" : "NIE", (unsigned)(ESP.getPsramSize() / 1024));

    settings_load();
    Serial.printf("Zrodlo danych: %s\n", data_source_name(g_settings.data_source));
    display_init();
    display_set_brightness(g_settings.brightness);
    clock_init(); // po dotyku - opcjonalny DS3231 na tej samej magistrali I2C
    telemetry_init();
    logger_init(); // po ekranie - karta SD dzieli piny 47/48 z SPI inicjalizacji ST7701
    ui_init();
    telemetry_start();
}

// loop() działa na rdzeniu 1 - obsługa grafiki LVGL
void loop() {
    lv_timer_handler();
    delay(5);
}
