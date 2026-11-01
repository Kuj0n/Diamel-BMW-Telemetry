#include "ui_internal.h"
#include <Arduino.h>
#include "settings.h"

// --- Strony Odczytów (3, przesuwane palcem): same wartości liczbowe, 2 kolumny x 6 wierszy ---
// Każda strona ma własną listę (do 12 wartości) - te same wartości mogą być na kilku stronach.
#define CELL_W 220
#define CELL_H 72

struct LivePage {
    lv_obj_t *obj;
    // Stan z chwili budowy strony - odświeżanie nie zależy od późniejszych zmian w g_settings
    lv_obj_t *lbl_value[PAGE_MAX];
    uint16_t ids[PAGE_MAX];
    int count;
};
static LivePage pages[LIVE_PAGES];

void live_build(int page, lv_obj_t *obj) {
    pages[page].obj = obj;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE); // 12 wartości mieści się bez przewijania
    live_rebuild(page);
}

void live_rebuild(int page) {
    LivePage &lp = pages[page];
    lv_obj_clean(lp.obj);
    lp.count = g_settings.page_count[page];
    memcpy(lp.ids, g_settings.page_ids[page], sizeof(lp.ids));

    for (int pos = 0; pos < lp.count; pos++) {
        uint16_t id = lp.ids[pos];
        lv_obj_t *cell = plain_cont(lp.obj);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_pos(cell, 20 + (pos % 2) * 235, 4 + (pos / 2) * CELL_H);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *name = make_label(cell, &lv_font_montserrat_16, module_color(param(id).module), param(id).name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT); // długie nazwy z katalogu
        lv_obj_set_width(name, CELL_W);

        lv_obj_t *row = plain_cont(cell);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

        lp.lbl_value[pos] = make_label(row, &lv_font_montserrat_32, COLOR_TEXT, "--");
        lv_obj_t *unit = make_label(row, &lv_font_montserrat_16, COLOR_DIM, param(id).unit);
        lv_obj_set_style_pad_left(unit, 6, 0);
        lv_obj_set_style_pad_bottom(unit, 5, 0);
    }

    if (lp.count == 0) {
        lv_obj_t *l = make_label(lp.obj, &lv_font_montserrat_20, COLOR_DIM, "");
        lv_label_set_text_fmt(l, "Strona %d jest pusta.\n\nWartosci dodasz w:\nUstawienia > Odczyty > Strona %d",
                              page + 1, page + 1);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
}

void live_rebuild_all() {
    for (int p = 0; p < LIVE_PAGES; p++) live_rebuild(p);
}

void live_refresh(int page) {
    LivePage &lp = pages[page];
    char buf[24];
    for (int pos = 0; pos < lp.count; pos++) {
        format_value(buf, sizeof(buf), lp.ids[pos]);
        set_text_if_changed(lp.lbl_value[pos], buf);
    }
}

// --- Edytor: wybór strony, lista jej wartości (kolejność ↑↓, usuwanie ✕), dodawanie z wyszukiwarki ---
// Wiersze tworzone raz (PAGE_MAX), nadmiarowe ukryte. Każda zmiana od razu przebudowuje stronę Odczytów.
static int edit_page = 0;
static lv_obj_t *tab_btn[LIVE_PAGES];
static lv_obj_t *row_obj[PAGE_MAX];
static lv_obj_t *row_label[PAGE_MAX];
static lv_obj_t *btn_add;
static lv_obj_t *lbl_count;

static void editor_refresh();

static void editor_changed() {
    editor_refresh();
    live_rebuild(edit_page);
    settings_mark_dirty();
}

static void editor_refresh() {
    uint8_t count = g_settings.page_count[edit_page];
    for (int i = 0; i < PAGE_MAX; i++) {
        if (i < count) {
            uint16_t id = g_settings.page_ids[edit_page][i];
            lv_label_set_text_fmt(row_label[i], "%d. %s", i + 1, param(id).name);
            lv_obj_clear_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int p = 0; p < LIVE_PAGES; p++) {
        lv_obj_set_style_bg_color(tab_btn[p], p == edit_page ? COLOR_ACCENT : COLOR_BTN, 0);
    }
    lv_label_set_text_fmt(lbl_count, "Strona %d: %d / %d %s", edit_page + 1, count, PAGE_MAX,
                          count ? "" : "- pusta (dodaj wartosci)");
    if (count >= PAGE_MAX) lv_obj_add_state(btn_add, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn_add, LV_STATE_DISABLED);
}

static void tab_cb(lv_event_t *e) {
    edit_page = (int)(intptr_t)lv_event_get_user_data(e);
    editor_refresh();
}

static void swap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int j = i + (lv_obj_get_user_data(lv_event_get_target(e)) ? 1 : -1); // user_data przycisku: 1 = w dół
    if (j < 0 || j >= g_settings.page_count[edit_page]) return;
    uint16_t *ids = g_settings.page_ids[edit_page];
    uint16_t t = ids[i];
    ids[i] = ids[j];
    ids[j] = t;
    editor_changed();
}

static void remove_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t *ids = g_settings.page_ids[edit_page];
    uint8_t &count = g_settings.page_count[edit_page];
    for (int k = i; k < count - 1; k++) ids[k] = ids[k + 1];
    count--;
    editor_changed();
}

// Bez powtórzeń w obrębie jednej strony (na innych stronach wolno)
static bool is_selected(uint16_t id) {
    for (int i = 0; i < g_settings.page_count[edit_page]; i++) {
        if (g_settings.page_ids[edit_page][i] == id) return true;
    }
    return false;
}

static void picked_cb(uint16_t id, void *) {
    uint8_t &count = g_settings.page_count[edit_page];
    if (id == PARAM_NONE || count >= PAGE_MAX || is_selected(id)) return;
    g_settings.page_ids[edit_page][count++] = id;
    editor_changed();
}

static void add_cb(lv_event_t *) {
    picker_open(false, is_selected, picked_cb, NULL, PAGE_LIVE_EDIT);
}

static void done_cb(lv_event_t *) {
    ui_show(PAGE_SETTINGS);
}

void live_editor_build(lv_obj_t *page) {
    for (int p = 0; p < LIVE_PAGES; p++) {
        char text[4];
        snprintf(text, sizeof(text), "%d", p + 1);
        tab_btn[p] = make_button(page, text, tab_cb, (void *)(intptr_t)p);
        lv_obj_set_size(tab_btn[p], 54, 44);
        lv_obj_set_pos(tab_btn[p], 10 + p * 60, 5);
    }
    btn_add = make_button(page, LV_SYMBOL_PLUS " Dodaj", add_cb, NULL);
    lv_obj_set_size(btn_add, 130, 44);
    lv_obj_set_pos(btn_add, 196, 5);
    lv_obj_t *done = make_button(page, "Gotowe", done_cb, NULL);
    lv_obj_set_size(done, 130, 44);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -10, 5);

    lbl_count = make_label(page, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_pos(lbl_count, 14, 56);

    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - 78);
    lv_obj_set_pos(list, 0, 78);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < PAGE_MAX; i++) {
        lv_obj_t *row = plain_cont(list);
        row_obj[i] = row;
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);

        row_label[i] = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, "");
        lv_label_set_long_mode(row_label[i], LV_LABEL_LONG_DOT); // długie nazwy z katalogu nie wchodzą na przyciski
        lv_obj_set_width(row_label[i], 270);
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
