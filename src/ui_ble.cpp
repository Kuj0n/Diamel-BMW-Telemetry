#include "ui_internal.h"
#include <Arduino.h>
#include "ble_obd.h"
#include "settings.h"

// --- Strona Adapter BLE: wyszukiwanie, wybór i test adaptera ELM327 ---
static lv_obj_t *btn_scan;
static lv_obj_t *btn_connect;
static lv_obj_t *btn_test;
static lv_obj_t *lbl_saved;
static lv_obj_t *lbl_status;
static lv_obj_t *list;
static lv_obj_t *lbl_test;
static BleFound shown[BLE_FOUND_MAX];
static int shown_count = -1;
static BleState prev_state = BLE_OFF;

static void saved_refresh() {
    char buf[64];
    if (g_settings.ble_addr[0]) snprintf(buf, sizeof(buf), "Wybrany: %s (%s)", g_settings.ble_name, g_settings.ble_addr);
    else snprintf(buf, sizeof(buf), "Wybrany: brak - wybierz z listy po wyszukaniu");
    set_text_if_changed(lbl_saved, buf);
}

static void scan_cb(lv_event_t *) {
    ble_scan();
}

static void connect_cb(lv_event_t *) {
    if (ble_state() == BLE_CONNECTED) ble_disconnect();
    else if (g_settings.ble_addr[0]) ble_connect(g_settings.ble_addr, g_settings.ble_addr_type);
    else ui_toast("Najpierw wybierz adapter z listy");
}

static void test_cb(lv_event_t *) {
    ble_test();
}

// Wybór z listy: zapamiętanie adaptera i połączenie
static void pick_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= shown_count) return;
    strlcpy(g_settings.ble_addr, shown[i].addr, sizeof(g_settings.ble_addr));
    strlcpy(g_settings.ble_name, shown[i].name, sizeof(g_settings.ble_name));
    g_settings.ble_addr_type = shown[i].addr_type;
    settings_mark_dirty();
    saved_refresh();
    settings_page_sync();
    ble_connect(g_settings.ble_addr, g_settings.ble_addr_type);
}

static void list_rebuild() {
    lv_obj_clean(list);
    shown_count = ble_found(shown, BLE_FOUND_MAX);
    if (shown_count == 0) {
        make_label(list, &lv_font_montserrat_16, COLOR_DIM,
                   "Nic nie znaleziono. Adapter musi byc w gniezdzie OBD (zasilanie) i miec Bluetooth LE.");
        return;
    }
    for (int i = 0; i < shown_count; i++) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
        lv_obj_add_event_cb(row, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, shown[i].name);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *r = make_label(row, &lv_font_montserrat_16, COLOR_DIM, "");
        lv_label_set_text_fmt(r, "%d dBm", shown[i].rssi);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void enable(lv_obj_t *btn, bool on) {
    if (on) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else lv_obj_add_state(btn, LV_STATE_DISABLED);
}

void ble_page_refresh() {
    BleState st = ble_state();
    char buf[200];
    ble_status_text(buf, sizeof(buf));
    set_text_if_changed(lbl_status, buf);
    ble_test_text(buf, sizeof(buf));
    set_text_if_changed(lbl_test, buf);

    bool busy = st == BLE_SCANNING || st == BLE_CONNECTING || st == BLE_TESTING;
    enable(btn_scan, !busy);
    enable(btn_connect, !busy);
    enable(btn_test, st == BLE_CONNECTED);
    set_text_if_changed(lv_obj_get_child(btn_connect, 0), st == BLE_CONNECTED ? "Rozlacz" : "Polacz");

    if (prev_state == BLE_SCANNING && st != BLE_SCANNING) list_rebuild();
    prev_state = st;
}

void ble_page_open() {
    saved_refresh();
    if (shown_count < 0) {
        make_label(list, &lv_font_montserrat_16, COLOR_DIM, "Nacisnij \"Szukaj\".");
        shown_count = 0;
    }
    ble_page_refresh();
}

void ble_page_build(lv_obj_t *page) {
    btn_scan = make_button(page, LV_SYMBOL_REFRESH " Szukaj", scan_cb, NULL);
    lv_obj_set_size(btn_scan, 140, 44);
    lv_obj_set_pos(btn_scan, 10, 6);
    btn_connect = make_button(page, "Polacz", connect_cb, NULL);
    lv_obj_set_size(btn_connect, 140, 44);
    lv_obj_set_pos(btn_connect, 160, 6);
    btn_test = make_button(page, "Test", test_cb, NULL);
    lv_obj_set_size(btn_test, 150, 44);
    lv_obj_align(btn_test, LV_ALIGN_TOP_RIGHT, -10, 6);

    lbl_saved = make_label(page, &lv_font_montserrat_16, COLOR_TEXT, "");
    lv_obj_set_pos(lbl_saved, 12, 58);
    lbl_status = make_label(page, &lv_font_montserrat_16, COLOR_ACCENT, "");
    lv_obj_set_pos(lbl_status, 12, 80);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, 190);
    lv_obj_set_pos(list, 0, 104);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lbl_test = make_label(page, &lv_font_montserrat_16, COLOR_TEXT, "");
    lv_obj_set_width(lbl_test, SCREEN_W - 24);
    lv_label_set_long_mode(lbl_test, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_test, 12, 298);

    lv_obj_t *hint = make_label(page, &lv_font_montserrat_16, COLOR_FAINT,
                                "Tylko adaptery z Bluetooth LE (np. vLinker MC+, OBDLink CX). "
                                "Test wysyla wylacznie komendy adaptera (AT), nie do auta.");
    lv_obj_set_width(hint, SCREEN_W - 24);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -6);
}
