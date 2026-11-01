#include "ui_internal.h"
#include <Arduino.h>
#include "settings.h"

// --- Strona Odczyty: same wartości liczbowe, 2 kolumny, wybór i kolejność z ustawień ---
#define CELL_W 220
#define CELL_H 70

static lv_obj_t *live_page;
static lv_obj_t *lbl_value[LIVE_MAX]; // indeks = pozycja na liście g_settings.live_ids

static lv_color_t group_color(ParamGroup g) {
    switch (g) {
        case GROUP_XDRIVE: return lv_color_hex(0x64B5F6);
        case GROUP_AC:     return lv_color_hex(0x80CBC4);
        default:           return COLOR_DIM;
    }
}

void live_build(lv_obj_t *page) {
    live_page = page;
    lv_obj_set_scroll_dir(page, LV_DIR_VER); // przy wielu wartościach strona przewija się w pionie
    live_rebuild();
}

void live_rebuild() {
    lv_obj_clean(live_page);

    for (int pos = 0; pos < g_settings.live_count; pos++) {
        uint16_t id = g_settings.live_ids[pos];

        lv_obj_t *cell = plain_cont(live_page);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_pos(cell, 20 + (pos % 2) * 235, 8 + (pos / 2) * CELL_H);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);

        make_label(cell, &lv_font_montserrat_16, group_color(PARAM_INFO[id].group), PARAM_INFO[id].name);

        lv_obj_t *row = plain_cont(cell);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

        lbl_value[pos] = make_label(row, &lv_font_montserrat_32, COLOR_TEXT, "--");
        lv_obj_t *unit = make_label(row, &lv_font_montserrat_16, COLOR_DIM, PARAM_INFO[id].unit);
        lv_obj_set_style_pad_left(unit, 6, 0);
        lv_obj_set_style_pad_bottom(unit, 5, 0);
    }

    if (g_settings.live_count == 0) {
        lv_obj_t *l = make_label(live_page, &lv_font_montserrat_20, COLOR_DIM,
                                 "Brak wybranych wartosci.\nUstawienia > Odczyty.");
        lv_obj_center(l);
    }
}

void live_refresh(const TelemetrySnapshot &s) {
    char buf[24];
    for (int pos = 0; pos < g_settings.live_count; pos++) {
        format_value(buf, sizeof(buf), (ParamId)g_settings.live_ids[pos], s);
        set_text_if_changed(lbl_value[pos], buf);
    }
}

// --- Edytor: lista wybranych wartości (kolejność ↑↓, usuwanie ✕) + dodawanie z wyszukiwarki ---
// Wiersze tworzone raz (LIVE_MAX), nadmiarowe ukryte - zmiany nie resetują przewinięcia listy.
static lv_obj_t *row_obj[LIVE_MAX];
static lv_obj_t *row_label[LIVE_MAX];
static lv_obj_t *btn_add;
static lv_obj_t *lbl_count;

static void editor_refresh() {
    for (int i = 0; i < LIVE_MAX; i++) {
        if (i < g_settings.live_count) {
            uint16_t id = g_settings.live_ids[i];
            lv_label_set_text_fmt(row_label[i], "%d. %s", i + 1, PARAM_INFO[id].name);
            lv_obj_clear_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text_fmt(lbl_count, "Wybrane: %d / %d", g_settings.live_count, LIVE_MAX);
    if (g_settings.live_count >= LIVE_MAX) lv_obj_add_state(btn_add, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn_add, LV_STATE_DISABLED);
}

static void swap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int j = i + (lv_obj_get_user_data(lv_event_get_target(e)) ? 1 : -1); // user_data przycisku: 1 = w dół
    if (j < 0 || j >= g_settings.live_count) return;
    uint16_t t = g_settings.live_ids[i];
    g_settings.live_ids[i] = g_settings.live_ids[j];
    g_settings.live_ids[j] = t;
    editor_refresh();
    settings_mark_dirty();
}

static void remove_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = i; k < g_settings.live_count - 1; k++) g_settings.live_ids[k] = g_settings.live_ids[k + 1];
    g_settings.live_count--;
    editor_refresh();
    settings_mark_dirty();
}

static bool is_selected(uint16_t id) {
    for (int i = 0; i < g_settings.live_count; i++) {
        if (g_settings.live_ids[i] == id) return true;
    }
    return false;
}

static void picked_cb(uint16_t id, void *) {
    if (id == PARAM_NONE || g_settings.live_count >= LIVE_MAX || is_selected(id)) return;
    g_settings.live_ids[g_settings.live_count++] = id;
    editor_refresh();
    settings_mark_dirty();
}

static void add_cb(lv_event_t *) {
    picker_open(false, is_selected, picked_cb, NULL, PAGE_LIVE_EDIT);
}

static void done_cb(lv_event_t *) {
    live_rebuild();
    ui_show(PAGE_SETTINGS);
}

void live_editor_build(lv_obj_t *page) {
    btn_add = make_button(page, LV_SYMBOL_PLUS " Dodaj", add_cb, NULL);
    lv_obj_set_size(btn_add, 130, 44);
    lv_obj_set_pos(btn_add, 10, 5);
    lbl_count = make_label(page, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_align(lbl_count, LV_ALIGN_TOP_MID, 10, 18);
    lv_obj_t *done = make_button(page, "Gotowe", done_cb, NULL);
    lv_obj_set_size(done, 110, 44);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -10, 5);

    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - 55);
    lv_obj_set_pos(list, 0, 55);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < LIVE_MAX; i++) {
        lv_obj_t *row = plain_cont(list);
        row_obj[i] = row;
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);

        row_label[i] = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, "");
        lv_obj_align(row_label[i], LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *del = make_button(row, LV_SYMBOL_CLOSE, remove_cb, (void *)(intptr_t)i);
        lv_obj_set_size(del, 50, 40);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x5D1F1F), 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);

        lv_obj_t *down = make_button(row, LV_SYMBOL_DOWN, swap_cb, (void *)(intptr_t)i);
        lv_obj_set_user_data(down, (void *)1);
        lv_obj_set_size(down, 50, 40);
        lv_obj_align(down, LV_ALIGN_RIGHT_MID, -60, 0);

        lv_obj_t *up = make_button(row, LV_SYMBOL_UP, swap_cb, (void *)(intptr_t)i);
        lv_obj_set_size(up, 50, 40);
        lv_obj_align(up, LV_ALIGN_RIGHT_MID, -116, 0);
    }
}

void live_editor_open() {
    editor_refresh();
}
