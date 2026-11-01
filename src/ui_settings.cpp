#include "ui_internal.h"
#include <Arduino.h>
#include "display.h"
#include "backup.h"
#include "clock.h"
#include "power.h"
#include "settings.h"
#include "wifi_web.h"

// --- Strona Ustawienia: przewijana lista, każda zmiana zapisywana automatycznie ---

// Pole liczbowe z przyciskami -/+ (przytrzymanie = szybka zmiana)
struct StepperDef {
    const char *name;
    uint16_t *value;
    uint16_t min, max, step;
    const char *unit;
    const char *zero_text; // tekst dla wartości 0 (np. "wyl."), NULL = pokaż liczbę
    lv_obj_t *label;
};

static StepperDef can_steppers[] = {
    {"Odstep zapytan",   &g_settings.poll_interval_ms, 10, 1000, 10, "ms", NULL, NULL},
    {"Timeout odczytu",  &g_settings.poll_timeout_ms,  10, 1000, 10, "ms", NULL, NULL},
    {"Timeout bledow",   &g_settings.dtc_timeout_ms,  500, 10000, 500, "ms", NULL, NULL},
};

static StepperDef log_steppers[] = {
    {"Interwal zapisu", &g_settings.log_interval_ms, 50, 5000, 50, "ms", NULL, NULL},
};

static StepperDef sleep_steppers[] = {
    {"Opoznienie", &g_settings.sleep_delay_s, 10, 3600, 10, "s", NULL, NULL},
};

static void stepper_update(StepperDef *d) {
    if (*d->value == 0 && d->zero_text) lv_label_set_text(d->label, d->zero_text);
    else lv_label_set_text_fmt(d->label, "%u %s", *d->value, d->unit);
}

static void stepper_cb(lv_event_t *e) {
    StepperDef *d = (StepperDef *)lv_event_get_user_data(e);
    bool plus = lv_obj_get_user_data(lv_event_get_target(e)) != NULL;
    int32_t v = *d->value + (plus ? d->step : -(int32_t)d->step);
    *d->value = (uint16_t)constrain(v, (int32_t)d->min, (int32_t)d->max);
    stepper_update(d);
    settings_mark_dirty();
}

static lv_obj_t *add_row(lv_obj_t *list, const char *name) {
    lv_obj_t *row = plain_cont(list);
    lv_obj_set_size(row, lv_pct(100), 50);
    lv_obj_t *l = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void add_section(lv_obj_t *list, const char *title) {
    lv_obj_t *l = make_label(list, &lv_font_montserrat_16, COLOR_ACCENT, title);
    lv_obj_set_style_pad_top(l, 14, 0);
}

static void add_stepper(lv_obj_t *list, StepperDef *d) {
    lv_obj_t *row = add_row(list, d->name);

    lv_obj_t *plus = make_button(row, LV_SYMBOL_PLUS, NULL, NULL);
    lv_obj_set_size(plus, 50, 40);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_user_data(plus, (void *)1);

    d->label = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_set_width(d->label, 100);
    lv_obj_set_style_text_align(d->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d->label, LV_ALIGN_RIGHT_MID, -55, 0);

    lv_obj_t *minus = make_button(row, LV_SYMBOL_MINUS, NULL, NULL);
    lv_obj_set_size(minus, 50, 40);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -160, 0);

    for (lv_obj_t *b : {plus, minus}) {
        lv_obj_add_event_cb(b, stepper_cb, LV_EVENT_SHORT_CLICKED, d);
        lv_obj_add_event_cb(b, stepper_cb, LV_EVENT_LONG_PRESSED_REPEAT, d);
    }
    stepper_update(d);
}

static lv_obj_t *add_dropdown(lv_obj_t *list, const char *name, const char *options, uint16_t selected,
                              lv_event_cb_t cb, void *user_data) {
    lv_obj_t *row = add_row(list, name);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_width(dd, 230);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return dd;
}

static void brightness_cb(lv_event_t *e) {
    g_settings.brightness = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    display_set_brightness(g_settings.brightness);
    settings_mark_dirty();
}

static void start_screen_cb(lv_event_t *e) {
    g_settings.start_screen = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
}

static void data_source_cb(lv_event_t *e) {
    g_settings.data_source = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
    if (g_settings.data_source != SRC_DEMO) ui_toast("Odczyt z auta - w przygotowaniu");
}

// Ptaszek zapisujący 0/1 w polu ustawień (user_data = wskaźnik na pole)
static void checkbox_cb(lv_event_t *e) {
    uint8_t *field = (uint8_t *)lv_event_get_user_data(e);
    *field = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_mark_dirty();
}

static void add_checkbox(lv_obj_t *list, const char *text, uint8_t *field) {
    lv_obj_t *cb = lv_checkbox_create(list);
    lv_checkbox_set_text(cb, text);
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_ver(cb, 8, 0);
    if (*field) lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_add_event_cb(cb, checkbox_cb, LV_EVENT_VALUE_CHANGED, field);
}

static lv_obj_t *dd_csv;
static lv_obj_t *dd_log_page;

static void log_page_cb(lv_event_t *e) {
    g_settings.log_page = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
}

static void csv_format_cb(lv_event_t *e) {
    g_settings.csv_format = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
}

// Serie wykresu wybierane wyszukiwarką; przycisk pokazuje aktualny wybór
static lv_obj_t *series_btn_label[CHART_SERIES_MAX];

static void series_label_update(int i) {
    uint16_t id = g_settings.chart_series[i];
    lv_label_set_text(series_btn_label[i], id == PARAM_NONE ? "(brak)" : param(id).name);
}

static void series_picked_cb(uint16_t id, void *ctx) {
    int i = (int)(intptr_t)ctx;
    g_settings.chart_series[i] = id;
    series_label_update(i);
    chart_rebuild();
    settings_mark_dirty();
}

static void series_btn_cb(lv_event_t *e) {
    picker_open(true, NULL, series_picked_cb, lv_event_get_user_data(e), PAGE_SETTINGS);
}

// Wybrany adapter BLE na przycisku; dotknięcie otwiera stronę wyszukiwania
static lv_obj_t *ble_btn_label;

static void ble_label_update() {
    lv_label_set_text(ble_btn_label, g_settings.ble_addr[0] ? g_settings.ble_name : "Wybierz " LV_SYMBOL_RIGHT);
}

static void open_ble_cb(lv_event_t *) {
    ui_show(PAGE_BLE);
}

// --- Kopia ustawień (JSON na karcie SD) ---
static lv_obj_t *btn_restore;

static void backup_save_cb(lv_event_t *) {
    char name[32], msg[64];
    if (backup_save_sd(name, sizeof(name))) snprintf(msg, sizeof(msg), "Zapisano: %s (Pliki)", name);
    else snprintf(msg, sizeof(msg), "Nie zapisano - brak karty SD?");
    ui_toast(msg);
}

static void restart_later(lv_timer_t *) {
    wifi_prepare_restart();
    ESP.restart();
}

// Drugie dotknięcie potwierdza; po przywróceniu restart (strony budują się od nowa)
static void backup_restore_cb(lv_event_t *) {
    if (!lv_obj_get_user_data(btn_restore)) {
        lv_obj_set_user_data(btn_restore, (void *)1);
        lv_obj_set_style_bg_color(btn_restore, lv_color_hex(0xB71C1C), 0);
        lv_label_set_text(lv_obj_get_child(btn_restore, 0), "Na pewno?");
        return;
    }
    lv_obj_set_user_data(btn_restore, NULL);
    lv_obj_set_style_bg_color(btn_restore, COLOR_BTN, 0);
    lv_label_set_text(lv_obj_get_child(btn_restore, 0), "Przywroc");
    char name[32], err[64], msg[100];
    if (!backup_restore_sd(name, sizeof(name), err, sizeof(err))) {
        ui_toast(err);
        return;
    }
    snprintf(msg, sizeof(msg), "Przywrocono %s%s%s - restart", name, err[0] ? ". " : "", err);
    ui_toast(msg);
    lv_timer_t *t = lv_timer_create(restart_later, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// --- Data i godzina (ręcznie; zwykle ustawia ją przeglądarka) ---
static lv_obj_t *clock_label;

static void set_time_value(double v) {
    int hhmm = (int)v;
    int h = hhmm / 100, m = hhmm % 100;
    if (h > 23 || m > 59) return ui_toast("Bledna godzina (GGMM, np. 1532)");
    struct tm tm;
    if (!clock_now(tm)) { // bez daty: zaczynamy od 2026-01-01
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = 126;
        tm.tm_mday = 1;
    }
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = 0;
    clock_set(mktime(&tm));
}

static void set_date_value(double v) {
    long d = (long)v; // DDMMRRRR, np. 01112026
    int day = d / 1000000, mon = (d / 10000) % 100, year = d % 10000;
    if (day < 1 || day > 31 || mon < 1 || mon > 12 || year < 2024 || year > 2099) {
        return ui_toast("Bledna data (DDMMRRRR, np. 01112026)");
    }
    struct tm tm;
    if (!clock_now(tm)) memset(&tm, 0, sizeof(tm));
    tm.tm_mday = day;
    tm.tm_mon = mon - 1;
    tm.tm_year = year - 1900;
    clock_set(mktime(&tm));
}

static void set_time_cb(lv_event_t *) {
    struct tm tm;
    bool ok = clock_now(tm);
    ui_number_input("Godzina GGMM (np. 1532)", ok ? tm.tm_hour * 100 + tm.tm_min : 0, 0, set_time_value);
}

static void set_date_cb(lv_event_t *) {
    struct tm tm;
    bool ok = clock_now(tm);
    double v = ok ? tm.tm_mday * 1000000.0 + (tm.tm_mon + 1) * 10000.0 + tm.tm_year + 1900 : 0;
    ui_number_input("Data DDMMRRRR (np. 01112026)", v, 0, set_date_value);
}

// --- Automatyczne uśpienie ---
static lv_obj_t *sleep_param_label;
static lv_obj_t *sleep_threshold_label;
static lv_obj_t *sleep_status;

static void sleep_labels_update() {
    ParamIdx id = g_settings.sleep_param;
    bool ok = id != PARAM_NONE && id < params_count();
    lv_label_set_text(sleep_param_label, ok ? param(id).name : "(wybierz)");
    // snprintf, nie lv_label_set_text_fmt - formatowanie LVGL nie obsługuje liczb zmiennoprzecinkowych
    char buf[40];
    snprintf(buf, sizeof(buf), "%.*f %s", ok ? param(id).decimals : 1, g_settings.sleep_threshold,
             ok ? param(id).unit : "");
    lv_label_set_text(sleep_threshold_label, buf);
}

static void sleep_picked_cb(uint16_t id, void *) {
    g_settings.sleep_param = id;
    sleep_labels_update();
    settings_mark_dirty();
}

static void sleep_param_cb(lv_event_t *) {
    picker_open(false, NULL, sleep_picked_cb, NULL, PAGE_SETTINGS);
}

static void sleep_above_cb(lv_event_t *e) {
    g_settings.sleep_above = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_mark_dirty();
}

static void sleep_threshold_set(double v) {
    g_settings.sleep_threshold = (float)v;
    sleep_labels_update();
    settings_mark_dirty();
}

static void sleep_threshold_cb(lv_event_t *) {
    ParamIdx id = g_settings.sleep_param;
    uint8_t dec = id != PARAM_NONE && id < params_count() ? param(id).decimals : 1;
    ui_number_input("Prog usypiania", g_settings.sleep_threshold, dec, sleep_threshold_set);
}

static void open_live_editor_cb(lv_event_t *) {
    live_editor_open();
    ui_show(PAGE_LIVE_EDIT);
}

void settings_page_build(lv_obj_t *page) {
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 20, 0);
    lv_obj_set_style_pad_ver(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    // Źródło danych
    add_section(list, "ZRODLO DANYCH");
    add_dropdown(list, "Tryb pracy", "DEMO (symulacja)\nCAN - gniazdo OBD\nAdapter BLE (ELM327)",
                 g_settings.data_source, data_source_cb, NULL);
    lv_obj_t *ble_row = add_row(list, "Adapter BLE");
    lv_obj_t *ble_btn = make_button(ble_row, "", open_ble_cb, NULL);
    lv_obj_set_size(ble_btn, 230, 40);
    lv_obj_align(ble_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    ble_btn_label = lv_obj_get_child(ble_btn, 0);
    lv_label_set_long_mode(ble_btn_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ble_btn_label, 200);
    lv_obj_set_style_text_align(ble_btn_label, LV_TEXT_ALIGN_CENTER, 0);
    ble_label_update();
    add_checkbox(list, "Start komunikacji po uruchomieniu", &g_settings.comm_autostart);
    lv_obj_t *src_hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                                    "DEMO: wartosci symulowane do testow przy biurku. CAN: komunikacje "
                                    "wlacza kafelek Start w menu (albo automatycznie). Adapter BLE - odczyt w przygotowaniu.");
    lv_label_set_long_mode(src_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(src_hint, lv_pct(100));

    // Kopia ustawień
    add_section(list, "KOPIA USTAWIEN (KARTA SD)");
    lv_obj_t *br = add_row(list, "Plik SET_*.json");
    lv_obj_t *bb = make_button(br, "Zapisz", backup_save_cb, NULL);
    lv_obj_set_size(bb, 110, 40);
    lv_obj_align(bb, LV_ALIGN_RIGHT_MID, -120, 0);
    btn_restore = make_button(br, "Przywroc", backup_restore_cb, NULL);
    lv_obj_set_size(btn_restore, 110, 40);
    lv_obj_align(btn_restore, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bh = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                              "Zapis: strony Odczytow, wykres, usypianie, Wi-Fi (z haslami) i reszta ustawien. "
                              "Przywraca najnowsza kopie z karty. Plik pobierzesz i wgrasz tez przez WWW.");
    lv_label_set_long_mode(bh, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bh, lv_pct(100));

    // Data i godzina
    add_section(list, "DATA I GODZINA");
    clock_label = make_label(list, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_t *r = add_row(list, "Ustaw");
    lv_obj_t *b = make_button(r, "Godzina", set_time_cb, NULL);
    lv_obj_set_size(b, 120, 40);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -130, 0);
    b = make_button(r, "Data", set_date_cb, NULL);
    lv_obj_set_size(b, 120, 40);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    add_checkbox(list, "Pobieraj czas z przegladarki (WWW)", &g_settings.time_from_browser);
    add_checkbox(list, "Modul zegara RTC (DS3231) zainstalowany", &g_settings.rtc_installed);
    // Stan modułu z chwili startu - zmiana ptaszka RTC działa po ponownym uruchomieniu
    const char *rtc_text;
    lv_color_t rtc_color = COLOR_FAINT;
    if (!g_settings.rtc_installed) {
        rtc_text = "Bez zegara z bateria czas ginie po odlaczeniu zasilania. Ustawia sie sam przy otwarciu "
                   "strony WWW (jesli zaznaczone) albo recznie.";
    } else if (clock_has_rtc()) {
        rtc_text = "Modul DS3231 wykryty - czas przetrwa wylaczenie zasilania.";
    } else {
        rtc_text = "Modulu DS3231 NIE WYKRYTO (I2C 0x68) - sprawdz podlaczenie. Zmiana dziala po restarcie.";
        rtc_color = COLOR_REC_ON;
    }
    lv_obj_t *clock_hint = make_label(list, &lv_font_montserrat_16, rtc_color, rtc_text);
    lv_label_set_long_mode(clock_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(clock_hint, lv_pct(100));

    // Ekran
    add_section(list, "EKRAN");
    make_label(list, &lv_font_montserrat_20, COLOR_TEXT, "Jasnosc");
    lv_obj_t *slider = lv_slider_create(list);
    lv_obj_set_width(slider, lv_pct(95));
    lv_obj_set_style_pad_ver(slider, 12, 0);
    lv_slider_set_range(slider, 10, 255);
    lv_slider_set_value(slider, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    add_dropdown(list, "Ekran startowy", "Menu\nOdczyty\nWykres\nBledy",
                 g_settings.start_screen, start_screen_cb, NULL);
    add_checkbox(list, "Pokaz FPS (klatki na sekunde)", &g_settings.show_fps);
    add_checkbox(list, "Pokaz obciazenie CPU", &g_settings.show_cpu);

    // Odczyty
    add_section(list, "ODCZYTY");
    lv_obj_t *row = add_row(list, "Strony 1-3 (po 12)");
    lv_obj_t *btn = make_button(row, "Zmien " LV_SYMBOL_RIGHT, open_live_editor_cb, NULL);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);

    // Wykres
    add_section(list, "WYKRES");
    static const char *series_names[CHART_SERIES_MAX] = {"Seria 1", "Seria 2", "Seria 3"};
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        lv_obj_t *r = add_row(list, series_names[i]);
        lv_obj_t *b = make_button(r, "", series_btn_cb, (void *)(intptr_t)i);
        lv_obj_set_size(b, 230, 40);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        series_btn_label[i] = lv_obj_get_child(b, 0);
        series_label_update(i);
    }
    lv_obj_t *hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                                "Inna jednostka niz Seria 1 = prawa os wykresu.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));

    // Logowanie
    add_section(list, "LOGOWANIE NA KARTE SD");
    for (StepperDef &d : log_steppers) add_stepper(list, &d);
    dd_csv = add_dropdown(list, "Format pliku", "Excel PL ( ; )\nStandard ( , )", g_settings.csv_format,
                          csv_format_cb, NULL);
    dd_log_page = add_dropdown(list, "Co zapisywac", "Strona 1\nStrona 2\nStrona 3\nWszystkie strony",
                               g_settings.log_page, log_page_cb, NULL);
    hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                      "Zapisywane sa wartosci z wybranej strony Odczytow (albo wszystkich, bez powtorzen) "
                      "i serie wykresu. Excel PL: separator srednik i przecinek dziesietny.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));

    // CAN
    add_section(list, "CAN");
    for (StepperDef &d : can_steppers) add_stepper(list, &d);

    // Automatyczne uśpienie: parametr, warunek, próg, opóźnienie
    add_section(list, "AUTOMATYCZNE USYPIANIE");
    add_checkbox(list, "Wlaczone", &g_settings.sleep_enabled);
    r = add_row(list, "Parametr");
    b = make_button(r, "", sleep_param_cb, NULL);
    lv_obj_set_size(b, 260, 40);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    sleep_param_label = lv_obj_get_child(b, 0);
    lv_label_set_long_mode(sleep_param_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sleep_param_label, 230);
    lv_obj_set_style_text_align(sleep_param_label, LV_TEXT_ALIGN_CENTER, 0);
    add_dropdown(list, "Warunek", "mniejszy niz\nwiekszy niz", g_settings.sleep_above, sleep_above_cb, NULL);
    r = add_row(list, "Prog");
    b = make_button(r, "", sleep_threshold_cb, NULL);
    lv_obj_set_size(b, 230, 40);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    sleep_threshold_label = lv_obj_get_child(b, 0);
    for (StepperDef &d : sleep_steppers) add_stepper(list, &d);
    add_checkbox(list, "Brak danych = warunek spelniony", &g_settings.sleep_on_missing);
    sleep_status = make_label(list, &lv_font_montserrat_16, COLOR_ACCENT, "");
    hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                      "Gdy warunek trwa przez caly czas opoznienia: koniec nagrywania i wygaszenie ekranu. "
                      "Dotyk budzi. Brak danych = np. zaplon wylaczony (sterownik nie odpowiada).");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_pad_bottom(hint, 20, 0);
    sleep_labels_update();
}

// Odliczanie do uśpienia (strona Ustawienia jest widoczna)
void settings_page_refresh() {
    char buf[80];
    clock_format(buf, sizeof(buf));
    set_text_if_changed(clock_label, buf);
    int left = power_seconds_left();
    if (!g_settings.sleep_enabled) {
        snprintf(buf, sizeof(buf), "Usypianie wylaczone");
    } else if (left >= 0) {
        snprintf(buf, sizeof(buf), "Warunek spelniony - uspienie za %d:%02d", left / 60, left % 60);
    } else {
        char v[32];
        format_value(v, sizeof(v), g_settings.sleep_param);
        snprintf(buf, sizeof(buf), "Warunek niespelniony (teraz: %s)", v);
    }
    set_text_if_changed(sleep_status, buf);
}

// Po zmianie ustawień z przeglądarki albo wyborze adaptera BLE
void settings_page_sync() {
    for (int i = 0; i < CHART_SERIES_MAX; i++) series_label_update(i);
    lv_dropdown_set_selected(dd_csv, g_settings.csv_format);
    lv_dropdown_set_selected(dd_log_page, g_settings.log_page);
    ble_label_update();
    sleep_labels_update();
}
