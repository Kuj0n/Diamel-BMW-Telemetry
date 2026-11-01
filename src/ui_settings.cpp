#include "ui_internal.h"
#include <Arduino.h>
#include "display.h"
#include "settings.h"

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

static StepperDef stop_steppers[] = {
    {"Brak odpowiedzi DDE", &g_settings.stop_no_reply_s,   2, 60, 1, "s",   NULL,   NULL},
    {"Obroty 0 przez",      &g_settings.stop_rpm_zero_min, 0, 60, 1, "min", "wyl.", NULL},
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

// Serie wykresu wybierane wyszukiwarką; przycisk pokazuje aktualny wybór
static lv_obj_t *series_btn_label[CHART_SERIES_MAX];

static void series_label_update(int i) {
    uint16_t id = g_settings.chart_series[i];
    lv_label_set_text(series_btn_label[i], id == PARAM_NONE ? "(brak)" : PARAM_INFO[id].name);
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

    // Odczyty
    add_section(list, "ODCZYTY");
    lv_obj_t *row = add_row(list, "Wartosci i kolejnosc");
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
    hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                      "Zapisywane sa wartosci ze strony Odczyty i z wykresu.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));

    // CAN
    add_section(list, "CAN");
    for (StepperDef &d : can_steppers) add_stepper(list, &d);

    add_section(list, "AUTOMATYCZNE ZATRZYMANIE CAN");
    for (StepperDef &d : stop_steppers) add_stepper(list, &d);
    hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                      "Brak odpowiedzi DDE = zaplon wylaczony. Obroty 0 - "
                      "dluzej niz postoj start-stop na swiatlach.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_pad_bottom(hint, 20, 0);
}
