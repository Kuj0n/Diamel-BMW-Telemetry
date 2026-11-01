#include <Arduino.h>
#include <lvgl.h>
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
    Serial.printf("Wersja: %s, tryb: %s\n", FW_VERSION, DEMO_MODE ? "DEMO" : "CAN");
    Serial.printf("PSRAM: %s, rozmiar: %u KB\n", psramFound() ? "TAK" : "NIE", (unsigned)(ESP.getPsramSize() / 1024));

    settings_load();
    display_init();
    display_set_brightness(g_settings.brightness);
    telemetry_init();
    logger_init(); // po ekranie - karta SD dzieli piny 47/48 z SPI inicjalizacji ST7701
    ui_init();

#if DEMO_MODE
    telemetry_start_demo();
#endif
}

// loop() działa na rdzeniu 1 - obsługa grafiki LVGL
void loop() {
    lv_timer_handler();
    delay(5);
}
