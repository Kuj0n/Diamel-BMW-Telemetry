#include "ui_internal.h"
#include <Arduino.h>
#include "settings.h"

// --- Wyszukiwarka parametrów: najpierw moduły (sterowniki), potem parametry modułu ---
// Pole wyszukiwania: na liście modułów szuka we wszystkich modułach, w module - tylko w nim.
// Wiersze tworzone dla jednej porcji wyników (katalog ma tysiące parametrów), dalej "Pokaz wiecej".
#define KEYBOARD_H 200
#define TOP_H 55
#define PAGE_SIZE 50

static lv_obj_t *search_ta;
static lv_obj_t *btn_back;
static lv_obj_t *keyboard;
static lv_obj_t *list;
static lv_obj_t *btn_more;

static bool allow_none;
static picker_exclude_t exclude_fn;
static picker_cb_t pick_cb;
static void *pick_ctx;
static Page return_page;
static int cur_module = -1; // -1 = lista modułów
static int next_offset;
static bool show_all = false;   // false = tylko lista podstawowa (zapamiętane do restartu)
static lv_obj_t *btn_all;
static bool ignore_text_change; // czyszczenie pola z kodu - przebudowa robiona osobno

static void rebuild();
static void clear_search();

static void rebuild_async(void *) {
    rebuild();
}

static void show_keyboard(bool show) {
    if (show) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(list, PAGE_H - TOP_H - KEYBOARD_H);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(list, PAGE_H - TOP_H);
    }
}

static void finish(uint16_t id, bool picked) {
    show_keyboard(false);
    if (picked && pick_cb) pick_cb(id, pick_ctx);
    ui_show(return_page);
}

static void param_row_cb(lv_event_t *e) {
    finish((uint16_t)(intptr_t)lv_event_get_user_data(e), true);
}

static void module_row_cb(lv_event_t *e) {
    cur_module = (int)(intptr_t)lv_event_get_user_data(e);
    clear_search();
    lv_async_call(rebuild_async, NULL); // przebudowa po obsłudze zdarzenia (usuwa kliknięty wiersz)
}

static void clear_search() {
    ignore_text_change = true;
    lv_textarea_set_text(search_ta, "");
    ignore_text_change = false;
}

static lv_obj_t *make_row(const char *left, const char *right, lv_color_t left_color) {
    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_size(row, lv_pct(100), 46);
    lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_t *l = make_label(row, &lv_font_montserrat_20, left_color, left);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, right ? 300 : 420);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (right) {
        lv_obj_t *r = make_label(row, &lv_font_montserrat_16, COLOR_DIM, right);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r, 110);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

static void more_cb(lv_event_t *);

// Kolejna porcja wyników wyszukiwania
static void append_page() {
    if (btn_more) {
        lv_obj_del(btn_more);
        btn_more = NULL;
    }
    const char *q = lv_textarea_get_text(search_ta);
    ParamIdx found[PAGE_SIZE];
    int total = params_search(cur_module, q, next_offset, found, PAGE_SIZE, show_all);
    int got = min(PAGE_SIZE, max(0, total - next_offset));
    for (int i = 0; i < got; i++) {
        ParamIdx id = found[i];
        if (exclude_fn && exclude_fn(id)) continue;
        const ParamDef &p = param(id);
        // W module po prawej jednostka, w wyszukiwaniu globalnym - moduł
        const char *right = cur_module >= 0 ? p.unit : module_def(p.module).name;
        lv_obj_t *row = make_row(p.name, right, cur_module >= 0 ? COLOR_TEXT : module_color(p.module));
        lv_obj_add_event_cb(row, param_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    }
    next_offset += got;
    if (total == 0) make_label(list, &lv_font_montserrat_20, COLOR_DIM, "Nic nie znaleziono.");
    if (next_offset < total) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Pokaz wiecej (%d z %d)", next_offset, total);
        btn_more = make_button(list, buf, more_cb, NULL);
        lv_obj_set_size(btn_more, lv_pct(100), 46);
    }
}

static void more_async(void *) {
    append_page();
}

static void more_cb(lv_event_t *) {
    lv_async_call(more_async, NULL);
}

static void none_cb(lv_event_t *) {
    finish(PARAM_NONE, true);
}

static void rebuild() {
    lv_obj_clean(list);
    btn_more = NULL;
    next_offset = 0;
    const char *q = lv_textarea_get_text(search_ta);

    if (cur_module < 0) lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_placeholder_text(search_ta, cur_module < 0 ? "Szukaj we wszystkich..." : "Szukaj w module...");

    if (cur_module < 0 && !*q) {
        if (allow_none) {
            lv_obj_t *row = make_row("(brak)", NULL, COLOR_DIM);
            lv_obj_add_event_cb(row, none_cb, LV_EVENT_CLICKED, NULL);
        }
        for (int m = 0; m < modules_count(); m++) {
            if (m == MODULE_DEMO && g_settings.data_source != SRC_DEMO) continue; // DEMO tylko w trybie DEMO
            const ModuleDef &md = module_def(m);
            uint16_t count = show_all ? md.count : md.basic_count;
            if (!count) continue;
            char right[16];
            snprintf(right, sizeof(right), "%u " LV_SYMBOL_RIGHT, count);
            lv_obj_t *row = make_row(md.name, right, module_color(m));
            lv_obj_add_event_cb(row, module_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        }
        return;
    }
    append_page();
}

static void ta_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED && !ignore_text_change) rebuild();
    else if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) show_keyboard(true);
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) show_keyboard(false);
}

static void back_cb(lv_event_t *) {
    cur_module = -1;
    clear_search();
    lv_async_call(rebuild_async, NULL);
}

// Lista podstawowa (kilkadziesiąt najważniejszych, po polsku) albo wszystkie parametry katalogu
static void all_label_update() {
    lv_label_set_text(lv_obj_get_child(btn_all, 0), show_all ? "Wszyst." : "Podst.");
    lv_obj_set_style_bg_color(btn_all, show_all ? COLOR_ACCENT : COLOR_BTN, 0);
}

static void all_cb(lv_event_t *) {
    show_all = !show_all;
    all_label_update();
    lv_async_call(rebuild_async, NULL);
}

static void cancel_cb(lv_event_t *) {
    finish(PARAM_NONE, false);
}

void picker_build(lv_obj_t *page) {
    btn_back = make_button(page, LV_SYMBOL_LEFT, back_cb, NULL);
    lv_obj_set_size(btn_back, 50, 46);
    lv_obj_set_pos(btn_back, 8, 5);

    search_ta = lv_textarea_create(page);
    lv_textarea_set_one_line(search_ta, true);
    lv_obj_set_style_text_font(search_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_size(search_ta, 190, 46);
    lv_obj_set_pos(search_ta, 66, 5);
    lv_obj_add_event_cb(search_ta, ta_cb, LV_EVENT_ALL, NULL);

    btn_all = make_button(page, "", all_cb, NULL);
    lv_obj_set_size(btn_all, 86, 46);
    lv_obj_set_pos(btn_all, 262, 5);
    lv_obj_set_style_text_font(lv_obj_get_child(btn_all, 0), &lv_font_montserrat_16, 0);
    all_label_update();

    lv_obj_t *cancel = make_button(page, "Anuluj", cancel_cb, NULL);
    lv_obj_set_size(cancel, 116, 46);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - TOP_H);
    lv_obj_set_pos(list, 0, TOP_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    keyboard = lv_keyboard_create(page);
    lv_obj_set_size(keyboard, SCREEN_W, KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, search_ta);
    lv_obj_add_event_cb(keyboard, kb_cb, LV_EVENT_ALL, NULL);
    show_keyboard(false);
}

void picker_open(bool none, picker_exclude_t exclude, picker_cb_t cb, void *ctx, Page ret) {
    allow_none = none;
    exclude_fn = exclude;
    pick_cb = cb;
    pick_ctx = ctx;
    return_page = ret;
    cur_module = -1;
    clear_search();
    rebuild();
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    ui_show(PAGE_PICKER);
}
