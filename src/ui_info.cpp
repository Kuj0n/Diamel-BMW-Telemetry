#include "ui_internal.h"
#include <Arduino.h>
#include <SD.h>
#include "can_test.h"
#include "catalog.h"
#include "comm.h"
#include "display.h"
#include "logger.h"
#include "settings.h"
#include "touch_gt911.h"
#include "wifi_web.h"

// --- Strona Info: wersja, autor, stan sprzętu i test toru CAN ---
static lv_obj_t *lbl_hw;
static lv_obj_t *lbl_runtime;
static lv_obj_t *lbl_can;
static lv_obj_t *btn_can;
static lv_obj_t *lbl_catalog;
static lv_obj_t *lbl_modules;
static lv_obj_t *btn_restart;

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
    // Test potrzebuje kontrolera CAN na wyłączność (i nie wolno go robić w aucie)
    if (!uds_release()) {
        ui_toast("Najpierw zatrzymaj komunikacje (Menu > Stop)");
        return;
    }
    lv_obj_add_state(btn_can, LV_STATE_DISABLED);
    lv_label_set_text(lbl_can, "Test w toku...");
    lv_obj_set_style_text_color(lbl_can, COLOR_DIM, 0);
    // Test blokuje na ~1 s - uruchamiamy po odświeżeniu ekranu, żeby napis zdążył się pokazać
    lv_timer_t *t = lv_timer_create(run_can_test, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// Restart (np. żeby wczytać nowy katalog) - drugie dotknięcie potwierdza
static void restart_cb(lv_event_t *) {
    if (logger_active()) {
        ui_toast("Najpierw zatrzymaj nagrywanie (REC)");
        return;
    }
    if (!lv_obj_get_user_data(btn_restart)) {
        lv_obj_set_user_data(btn_restart, (void *)1);
        lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0xB71C1C), 0);
        lv_label_set_text(lv_obj_get_child(btn_restart, 0), "Na pewno?");
        return;
    }
    settings_flush();
    wifi_prepare_restart();
    ESP.restart();
}

static void catalog_refresh() {
    CatalogStatus cs;
    catalog_status(cs);
    if (!cs.loaded) {
        lv_label_set_text_fmt(lbl_catalog, "Nie wczytano: %s\nDostepne tylko wartosci DEMO.",
                              cs.error[0] ? cs.error : "brak danych");
        lv_obj_set_style_text_color(lbl_catalog, COLOR_REC_ON, 0);
        lv_label_set_text(lbl_modules, "Pliki wgrasz przez WWW: System > Katalog parametrow.");
        return;
    }
    unsigned basic = 0;
    for (int m = 1; m < modules_count(); m++) basic += module_def(m).basic_count;
    char dtc[48];
    if (cs.dtc_codes) snprintf(dtc, sizeof(dtc), "%u (bledy.csv)", (unsigned)cs.dtc_codes);
    else snprintf(dtc, sizeof(dtc), "%u plikow bledy_XX.csv (stary uklad)", cs.dtc_files);
    // Lista podstawowa = wszystkie parametry -> katalog bez kolumny "poziom" (starsza wersja pliku)
    lv_label_set_text_fmt(lbl_catalog,
                          "%s\nParametry: %u z %u modulow, podstawowe: %u%s\nPominiete (tekst / bajty): %u\n"
                          "Opisy wartosci: %u (tabel: %u)\nKody bledow: %s\nCzas wczytania: %u ms%s%s",
                          cs.meta[0] ? cs.meta : "Katalog bez wersji (starszy - wygeneruj i wgraj nowy)",
                          (unsigned)cs.params, cs.modules, basic, basic == cs.params ? " (brak podzialu!)" : "",
                          (unsigned)cs.skipped, (unsigned)cs.texts, cs.text_tables, dtc, (unsigned)cs.load_ms,
                          cs.error[0] ? "\nUwaga: " : "", cs.error);
    lv_obj_set_style_text_color(lbl_catalog, COLOR_TEXT, 0);

    // Moduły: adres, nazwa, liczba parametrów i kodów błędów
    static char buf[1600];
    size_t o = 0;
    for (int m = 1; m < modules_count() && o < sizeof(buf) - 80; m++) {
        const ModuleDef &md = module_def(m);
        o += snprintf(buf + o, sizeof(buf) - o, "%s%02X  %s: %u param., %u bledow", o ? "\n" : "", md.addr, md.name,
                      md.count, md.dtc_count);
    }
    lv_label_set_text(lbl_modules, buf);
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

    add_section(list, "KATALOG PARAMETROW (KARTA SD /diamel)");
    lbl_catalog = add_text(list, COLOR_TEXT);
    lbl_modules = add_text(list, COLOR_DIM);
    btn_restart = make_button(list, LV_SYMBOL_REFRESH " Uruchom ponownie", restart_cb, NULL);
    lv_obj_set_size(btn_restart, 260, 46);
    lv_obj_t *restart_hint = add_text(list, COLOR_FAINT);
    lv_label_set_text(restart_hint, "Katalog wczytuje sie przy starcie - po wgraniu nowych plikow przez WWW uruchom ponownie.");

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
                          touch, sd, data_source_name(g_settings.data_source));
    catalog_refresh();
    lv_obj_set_user_data(btn_restart, NULL); // potwierdzenie restartu od nowa
    lv_obj_set_style_bg_color(btn_restart, COLOR_BTN, 0);
    lv_label_set_text(lv_obj_get_child(btn_restart, 0), LV_SYMBOL_REFRESH " Uruchom ponownie");
    info_page_refresh();
}

void info_page_refresh() {
    char line[96];
    uint32_t s = millis() / 1000;
    // RAM wewnętrzny: bieżący / najmniejszy od startu (poniżej ~20 KB grożą restarty)
    snprintf(line, sizeof(line), "Wolny PSRAM: %u KB\nRAM wewn.: %u KB (min. %u KB)\nCzas pracy: %u:%02u:%02u",
             (unsigned)(ESP.getFreePsram() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    set_text_if_changed(lbl_runtime, line);
}
