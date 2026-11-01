#include "ui_internal.h"
#include <Arduino.h>
#include <WiFi.h>
#include "settings.h"
#include "wifi_web.h"

// --- Strona "Siec domowa": wyszukanie sieci, nazwa i hasło z klawiatury ekranowej ---
#define KEYBOARD_H 200
#define LIST_Y 156

static lv_obj_t *ta_ssid;
static lv_obj_t *ta_pass;
static lv_obj_t *cb_show;
static lv_obj_t *keyboard;
static lv_obj_t *list;
static lv_obj_t *lbl_status;
static bool scanning = false;

static void show_keyboard(lv_obj_t *ta) {
    if (ta) {
        lv_keyboard_set_textarea(keyboard, ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) show_keyboard(lv_event_get_target(e));
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) show_keyboard(NULL);
}

static void show_cb(lv_event_t *) {
    lv_textarea_set_password_mode(ta_pass, !lv_obj_has_state(cb_show, LV_STATE_CHECKED));
}

static void pick_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    lv_textarea_set_text(ta_ssid, lv_label_get_text(lv_obj_get_child(row, 0)));
    lv_textarea_set_text(ta_pass, "");
    show_keyboard(ta_pass); // od razu hasło
}

// Wyszukiwanie wymaga trybu STA - przy wyłączonym Wi-Fi radio włącza się na czas szukania
static void scan_cb(lv_event_t *) {
    if (scanning) return;
    if (!wifi_active()) WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    scanning = true;
    lv_label_set_text(lbl_status, "Szukanie sieci...");
}

static void scan_results() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    scanning = false;
    lv_obj_clean(list);
    if (n <= 0) {
        lv_label_set_text(lbl_status, "Nie znaleziono sieci.");
    } else {
        lv_label_set_text_fmt(lbl_status, "Znaleziono sieci: %d - dotknij, aby wybrac", n);
        for (int i = 0; i < n && i < 20; i++) {
            String name = WiFi.SSID(i);
            if (!name.length()) continue;
            lv_obj_t *row = lv_btn_create(list);
            lv_obj_set_size(row, lv_pct(100), 42);
            lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
            lv_obj_add_event_cb(row, pick_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, name.c_str());
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_t *r = make_label(row, &lv_font_montserrat_16, COLOR_DIM, "");
            lv_label_set_text_fmt(r, "%d dBm%s", WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " otw." : "");
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
    WiFi.scanDelete();
    if (!wifi_active()) WiFi.mode(WIFI_OFF);
}

static void save_cb(lv_event_t *) {
    const char *ssid = lv_textarea_get_text(ta_ssid);
    if (!*ssid) {
        ui_toast("Podaj nazwe sieci");
        return;
    }
    strlcpy(g_settings.home_ssid, ssid, sizeof(g_settings.home_ssid));
    strlcpy(g_settings.home_pass, lv_textarea_get_text(ta_pass), sizeof(g_settings.home_pass));
    g_settings.wifi_mode = WIFI_MODE_HOME;
    settings_mark_dirty();
    show_keyboard(NULL);
    if (wifi_active()) wifi_restart();
    else wifi_start();
    ui_toast("Zapisano - laczenie z siecia domowa");
    ui_show(PAGE_WIFI);
}

static void back_cb(lv_event_t *) {
    show_keyboard(NULL);
    ui_show(PAGE_WIFI);
}

void wifi_home_page_refresh() {
    if (scanning) scan_results();
}

void wifi_home_page_open() {
    lv_textarea_set_text(ta_ssid, g_settings.home_ssid);
    lv_textarea_set_text(ta_pass, g_settings.home_pass);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_clear_state(cb_show, LV_STATE_CHECKED);
    show_keyboard(NULL);
}

void wifi_home_page_build(lv_obj_t *page) {
    lv_obj_t *b = make_button(page, LV_SYMBOL_LEFT, back_cb, NULL);
    lv_obj_set_size(b, 56, 44);
    lv_obj_set_pos(b, 8, 5);
    b = make_button(page, LV_SYMBOL_REFRESH " Szukaj", scan_cb, NULL);
    lv_obj_set_size(b, 150, 44);
    lv_obj_set_pos(b, 72, 5);
    b = make_button(page, "Zapisz i polacz", save_cb, NULL);
    lv_obj_set_size(b, 236, 44);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, 5);

    ta_ssid = lv_textarea_create(page);
    lv_textarea_set_one_line(ta_ssid, true);
    lv_textarea_set_max_length(ta_ssid, 32);
    lv_textarea_set_placeholder_text(ta_ssid, "Nazwa sieci (SSID)");
    lv_obj_set_style_text_font(ta_ssid, &lv_font_montserrat_20, 0);
    lv_obj_set_size(ta_ssid, SCREEN_W - 16, 44);
    lv_obj_set_pos(ta_ssid, 8, 56);
    lv_obj_add_event_cb(ta_ssid, ta_cb, LV_EVENT_ALL, NULL);

    ta_pass = lv_textarea_create(page);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_max_length(ta_pass, 64);
    lv_textarea_set_placeholder_text(ta_pass, "Haslo");
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_set_style_text_font(ta_pass, &lv_font_montserrat_20, 0);
    lv_obj_set_size(ta_pass, SCREEN_W - 140, 44);
    lv_obj_set_pos(ta_pass, 8, 106);
    lv_obj_add_event_cb(ta_pass, ta_cb, LV_EVENT_ALL, NULL);

    cb_show = lv_checkbox_create(page);
    lv_checkbox_set_text(cb_show, "Pokaz");
    lv_obj_set_style_text_font(cb_show, &lv_font_montserrat_16, 0);
    lv_obj_align(cb_show, LV_ALIGN_TOP_RIGHT, -12, 116);
    lv_obj_add_event_cb(cb_show, show_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_status = make_label(page, &lv_font_montserrat_16, COLOR_DIM, "Nacisnij \"Szukaj\" albo wpisz nazwe sieci.");
    lv_obj_set_pos(lbl_status, 12, LIST_Y);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - LIST_Y - 22);
    lv_obj_set_pos(list, 0, LIST_Y + 22);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    keyboard = lv_keyboard_create(page);
    lv_obj_set_size(keyboard, SCREEN_W, KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard, kb_cb, LV_EVENT_ALL, NULL);
    show_keyboard(NULL);
}
