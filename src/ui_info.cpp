#include "ui_internal.h"
#include <Arduino.h>
#include <SD.h>
#include "can_test.h"
#include "display.h"
#include "logger.h"
#include "touch_gt911.h"

// --- Strona Info: wersja, autor, stan sprzętu i test toru CAN ---
static lv_obj_t *lbl_hw;
static lv_obj_t *lbl_runtime;
static lv_obj_t *lbl_can;
static lv_obj_t *btn_can;

static void add_section(lv_obj_t *list, const char *title) {
    lv_obj_t *l = make_label(list, &lv_font_montserrat_16, COLOR_ACCENT, title);
    lv_obj_set_style_pad_top(l, 12, 0);
}

static lv_obj_t *add_text(lv_obj_t *list, lv_color_t color) {
    lv_obj_t *l = make_label(list, &lv_font_montserrat_16, color, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static void run_can_test(lv_timer_t *) {
    CanTestResult r;
    bool ok = can_self_test(r);
    if (!r.driver_ok) {
        lv_label_set_text(lbl_can, "BLAD: nie udalo sie uruchomic kontrolera CAN.");
        lv_obj_set_style_text_color(lbl_can, COLOR_REC_ON, 0);
    } else if (ok) {
        lv_label_set_text_fmt(lbl_can, "OK: odebrano %d/%d ramek.\nTX (IO40), transceiver i RX (IO2) dzialaja.",
                              r.received, r.sent);
        lv_obj_set_style_text_color(lbl_can, lv_color_hex(0x81C784), 0);
    } else {
        lv_label_set_text_fmt(lbl_can,
                              "BLAD: odebrano %d/%d ramek (bledy TX %u, RX %u%s).\n"
                              "Sprawdz: zasilanie 3.3V transceivera (S3), przewody TX/RX, "
                              "czy TX/RX nie sa zamienione. Bez auta pomaga tez tymczasowy "
                              "rezystor 120 Ohm miedzy CANH i CANL.",
                              r.received, r.sent, (unsigned)r.tx_errors, (unsigned)r.rx_errors,
                              r.bus_off ? ", bus off" : "");
        lv_obj_set_style_text_color(lbl_can, COLOR_REC_ON, 0);
    }
    lv_obj_clear_state(btn_can, LV_STATE_DISABLED);
}

static void can_test_cb(lv_event_t *) {
    lv_obj_add_state(btn_can, LV_STATE_DISABLED);
    lv_label_set_text(lbl_can, "Test w toku...");
    lv_obj_set_style_text_color(lbl_can, COLOR_DIM, 0);
    // Test blokuje na ~1 s - uruchamiamy po odświeżeniu ekranu, żeby napis zdążył się pokazać
    lv_timer_t *t = lv_timer_create(run_can_test, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void info_page_build(lv_obj_t *page) {
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 20, 0);
    lv_obj_set_style_pad_ver(list, 10, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    make_label(list, &lv_font_montserrat_32, COLOR_TEXT, "Diamel Telemetry");
    lv_obj_t *about = add_text(list, COLOR_DIM);
    lv_label_set_text_fmt(about, "Wersja %s (kompilacja %s)\nAutor: %s\nPojazd: %s",
                          FW_VERSION, __DATE__, FW_AUTHOR, FW_VEHICLE);

    add_section(list, "SPRZET");
    lbl_hw = add_text(list, COLOR_TEXT);
    lbl_runtime = add_text(list, COLOR_DIM);

    add_section(list, "MAGISTRALA CAN");
    lv_obj_t *can_info = add_text(list, COLOR_TEXT);
    lv_label_set_text_fmt(can_info, "Transceiver SN65HVD230, TX = IO%d, RX = IO%d, 500 kbps",
                          CAN_TX_PIN, CAN_RX_PIN);
    btn_can = make_button(list, "Test CAN", can_test_cb, NULL);
    lv_obj_set_size(btn_can, 200, 46);
    lbl_can = add_text(list, COLOR_DIM);
    lv_label_set_text(lbl_can, "Test sprawdza tor TX -> transceiver -> RX bez auta.");
    lv_obj_t *warn = add_text(list, COLOR_ACCENT);
    lv_label_set_text(warn, "Test nadaje ramki - uruchamiaj tylko z wtyczka OBD odlaczona od auta.");
    lv_obj_set_style_pad_bottom(warn, 20, 0);
}

void info_page_open() {
    char sd[64];
    if (logger_sd_ok()) {
        snprintf(sd, sizeof(sd), "OK, %u MB", (unsigned)(SD.cardSize() / (1024 * 1024)));
    } else {
        snprintf(sd, sizeof(sd), "brak");
    }
    char touch[32];
    if (display_touch_ok()) snprintf(touch, sizeof(touch), "GT911 (0x%02X)", gt911_address());
    else snprintf(touch, sizeof(touch), "brak");

    lv_label_set_text_fmt(lbl_hw, "Ekran: ST7701 480x480\nDotyk: %s\nKarta SD: %s\nTryb danych: %s",
                          touch, sd, DEMO_MODE ? "DEMO (symulacja)" : "CAN");
    info_page_refresh();
}

void info_page_refresh() {
    char line[96];
    uint32_t s = millis() / 1000;
    snprintf(line, sizeof(line), "Wolny PSRAM: %u KB, RAM: %u KB\nCzas pracy: %u:%02u:%02u",
             (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    set_text_if_changed(lbl_runtime, line);
}
