#include "ui_internal.h"
#include <Arduino.h>
#include "settings.h"
#include "wifi_web.h"

// --- Strona Wi-Fi: włączanie punktu dostępowego, dane sieci i kod QR do połączenia telefonu ---
#define QR_SIZE 170
#define COLOR_ON lv_color_hex(0x2E7D32)

static lv_obj_t *btn_toggle;
static lv_obj_t *lbl_toggle;
static lv_obj_t *lbl_info;
static lv_obj_t *qr;
static lv_obj_t *qr_box;
static lv_obj_t *btn_pass;
static bool starting = false;
static bool pass_confirm = false;

// Okno postępu aktualizacji OTA (nad wszystkimi stronami, blokuje dotyk)
static lv_obj_t *overlay;
static lv_obj_t *overlay_bar;
static lv_obj_t *overlay_lbl;

// Kod QR w formacie rozpoznawanym przez aparat telefonu (Android/iOS): dołącz do sieci
// W sieci domowej kod QR z adresem strony
static WifiState qr_state = WSTATE_OFF;
static lv_obj_t *dd_mode;

static void update_qr() {
    char text[80];
    if (wifi_state() == WSTATE_HOME) snprintf(text, sizeof(text), "http://%s", wifi_ip());
    else snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", wifi_ssid(), g_settings.wifi_pass);
    lv_qrcode_update(qr, text, strlen(text));
    qr_state = wifi_state();
}

static void set_pass_button(bool confirm) {
    pass_confirm = confirm;
    lv_obj_set_style_bg_color(btn_pass, confirm ? lv_color_hex(0xB71C1C) : COLOR_BTN, 0);
    lv_label_set_text(lv_obj_get_child(btn_pass, 0), confirm ? "Na pewno?" : "Nowe haslo");
}

void wifi_page_refresh() {
    bool on = wifi_active();
    WifiState st = wifi_state();
    char buf[200];
    if (starting) {
        snprintf(buf, sizeof(buf), "Uruchamianie...");
    } else if (st == WSTATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "Laczenie z siecia:\n%s\n(do 20 s, potem\nwlasna siec)", g_settings.home_ssid);
    } else if (st == WSTATE_HOME) {
        snprintf(buf, sizeof(buf), "Siec domowa:\n%s\nAdres:\nhttp://%s\nhttp://%s.local", g_settings.home_ssid,
                 wifi_ip(), MDNS_NAME);
    } else if (!on && g_settings.wifi_mode == WIFI_MODE_HOME) {
        snprintf(buf, sizeof(buf), "Wi-Fi wylaczone.\n\nSiec domowa:\n%s",
                 g_settings.home_ssid[0] ? g_settings.home_ssid : "nie ustawiona -\nprzycisk Siec");
    } else if (on) {
        snprintf(buf, sizeof(buf), "Siec:\n%s\nHaslo:\n%s\nAdres:\nhttp://%s\nPolaczone: %u", wifi_ssid(),
                 g_settings.wifi_pass, wifi_ip(), wifi_clients());
    } else {
        snprintf(buf, sizeof(buf), "Wi-Fi wylaczone.\n\nSiec:\n%s\nHaslo:\n%s", wifi_ssid(), g_settings.wifi_pass);
    }
    set_text_if_changed(lbl_info, buf);
    set_text_if_changed(lbl_toggle, on ? LV_SYMBOL_WIFI " Wylacz" : LV_SYMBOL_WIFI " Wlacz");
    lv_obj_set_style_bg_color(btn_toggle, on ? COLOR_ON : COLOR_BTN, 0);

    if (on && st != WSTATE_CONNECTING && st != qr_state) update_qr();
    if (on && st != WSTATE_CONNECTING) lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(qr_box, LV_OBJ_FLAG_HIDDEN);
    // Nowe hasło tylko przy wyłączonym Wi-Fi; w trakcie aktualizacji nie wyłączamy sieci
    if (on) lv_obj_add_state(btn_pass, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn_pass, LV_STATE_DISABLED);
    if (starting || wifi_ota_progress() >= 0) lv_obj_add_state(btn_toggle, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn_toggle, LV_STATE_DISABLED);
}

// Uruchomienie trwa ~0.5 s - po odświeżeniu ekranu, żeby napis "Uruchamianie..." zdążył się pokazać
static void start_later(lv_timer_t *) {
    bool ok = wifi_start();
    starting = false;
    if (ok) update_qr();
    else ui_toast("Nie udalo sie wlaczyc Wi-Fi");
    wifi_page_refresh();
}

static void toggle_cb(lv_event_t *) {
    if (wifi_active()) {
        wifi_stop();
    } else {
        starting = true;
        lv_timer_t *t = lv_timer_create(start_later, 50, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    set_pass_button(false);
    wifi_page_refresh();
}

static void home_cb(lv_event_t *) {
    ui_show(PAGE_WIFI_HOME);
}

// Tryb: własna sieć / sieć domowa (nazwę i hasło sieci domowej: przycisk "Siec" albo strona WWW)
static void mode_cb(lv_event_t *e) {
    g_settings.wifi_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
    if (g_settings.wifi_mode == WIFI_MODE_HOME && !g_settings.home_ssid[0]) {
        ui_toast("Ustaw siec domowa: przycisk Siec");
    }
    wifi_restart(); // tylko gdy Wi-Fi jest włączone
    wifi_page_refresh();
}

static void new_pass_cb(lv_event_t *) {
    if (!pass_confirm) {
        set_pass_button(true);
        return;
    }
    settings_new_wifi_password();
    set_pass_button(false);
    wifi_page_refresh();
    ui_toast("Nowe haslo zapisane");
}

void wifi_page_build(lv_obj_t *page) {
    btn_toggle = make_button(page, "", toggle_cb, NULL);
    lv_obj_set_size(btn_toggle, 200, 50);
    lv_obj_set_pos(btn_toggle, 20, 14);
    lbl_toggle = lv_obj_get_child(btn_toggle, 0);

    btn_pass = make_button(page, "", new_pass_cb, NULL);
    lv_obj_set_size(btn_pass, 200, 50);
    lv_obj_align(btn_pass, LV_ALIGN_TOP_RIGHT, -20, 14);
    set_pass_button(false);

    dd_mode = lv_dropdown_create(page);
    lv_dropdown_set_options(dd_mode, "Wlasna siec\nSiec domowa");
    lv_dropdown_set_selected(dd_mode, g_settings.wifi_mode);
    lv_obj_set_style_text_font(dd_mode, &lv_font_montserrat_16, 0);
    lv_obj_set_size(dd_mode, 150, 40);
    lv_obj_set_pos(dd_mode, 20, 72);
    lv_obj_add_event_cb(dd_mode, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    // Sieć domowa: nazwa i hasło z klawiatury ekranowej
    lv_obj_t *home_btn = make_button(page, "Siec", home_cb, NULL);
    lv_obj_set_size(home_btn, 80, 40);
    lv_obj_set_pos(home_btn, 176, 72);

    lbl_info = make_label(page, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_set_width(lbl_info, 240);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_info, 20, 122);

    // Biała ramka wokół kodu - telefony gorzej czytają kod bez jasnego marginesu
    qr_box = plain_cont(page);
    lv_obj_set_size(qr_box, QR_SIZE + 20, QR_SIZE + 20);
    lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(qr_box, 8, 0);
    lv_obj_align(qr_box, LV_ALIGN_TOP_RIGHT, -20, 84);
    qr = lv_qrcode_create(qr_box, QR_SIZE, lv_color_black(), lv_color_white());
    lv_obj_center(qr);
    update_qr();

    lv_obj_t *hint = make_label(page, &lv_font_montserrat_16, COLOR_FAINT,
                                "Wlasna siec: zeskanuj kod aparatem telefonu, potem otworz adres.\n"
                                "Siec domowa: kod QR prowadzi prosto do strony. Siec ustawisz przyciskiem Siec albo na "
                                "stronie WWW (System > Wi-Fi); gdy jej nie ma, po 20 s wlacza sie wlasna siec.");
    lv_obj_set_width(hint, SCREEN_W - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 20, -12);

    wifi_page_refresh();
}

void wifi_overlay_tick() {
    int p = wifi_ota_progress();
    if (p >= 0) {
        if (!overlay) {
            overlay = plain_cont(lv_layer_top());
            lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
            lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE); // pochłania dotyk
            lv_obj_set_style_bg_color(overlay, COLOR_BG, 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
            overlay_lbl = make_label(overlay, &lv_font_montserrat_20, COLOR_TEXT, "");
            lv_obj_set_style_text_align(overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(overlay_lbl, LV_ALIGN_CENTER, 0, -40);
            overlay_bar = lv_bar_create(overlay);
            lv_obj_set_size(overlay_bar, 360, 20);
            lv_obj_set_style_bg_color(overlay_bar, COLOR_ACCENT, LV_PART_INDICATOR);
            lv_obj_align(overlay_bar, LV_ALIGN_CENTER, 0, 20);
        }
        char buf[80];
        if (p < 100) snprintf(buf, sizeof(buf), "Aktualizacja oprogramowania\n%d%%  -  nie wylaczaj zasilania", p);
        else snprintf(buf, sizeof(buf), "Aktualizacja zakonczona.\nUruchamianie ponowne...");
        set_text_if_changed(overlay_lbl, buf);
        lv_bar_set_value(overlay_bar, p, LV_ANIM_OFF);
    } else if (overlay) {
        lv_obj_del(overlay);
        overlay = NULL;
    }

    const char *m = wifi_take_message();
    if (m) ui_toast(m);
}
