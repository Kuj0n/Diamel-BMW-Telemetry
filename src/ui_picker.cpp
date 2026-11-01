#include "ui_internal.h"
#include <Arduino.h>
#include <ctype.h>
#include "settings.h"

// --- Wyszukiwarka parametrów: pole tekstowe + klawiatura + przefiltrowana lista ---
// Wiersze tworzone raz; filtr tylko je ukrywa/pokazuje (szybko także przy setkach pozycji).
#define KEYBOARD_H 200
#define TOP_H 55

static lv_obj_t *search_ta;
static lv_obj_t *keyboard;
static lv_obj_t *list;
static lv_obj_t *row_none;
static lv_obj_t *rows[P_COUNT];
static lv_obj_t *lbl_empty;

static bool allow_none;
static picker_exclude_t exclude_fn;
static picker_cb_t pick_cb;
static void *pick_ctx;
static Page return_page;

// Dopasowanie bez rozróżniania wielkości liter
static bool contains_ci(const char *text, const char *needle) {
    if (!*needle) return true;
    for (; *text; text++) {
        const char *t = text, *n = needle;
        while (*t && *n && tolower((unsigned char)*t) == tolower((unsigned char)*n)) {
            t++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

static void apply_filter() {
    const char *q = lv_textarea_get_text(search_ta);
    int shown = 0;
    for (int id = 0; id < P_COUNT; id++) {
        bool show = !(exclude_fn && exclude_fn(id)) &&
                    (contains_ci(PARAM_INFO[id].name, q) || contains_ci(GROUP_NAMES[PARAM_INFO[id].group], q));
        if (show) {
            lv_obj_clear_flag(rows[id], LV_OBJ_FLAG_HIDDEN);
            shown++;
        } else {
            lv_obj_add_flag(rows[id], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (allow_none && !*q) lv_obj_clear_flag(row_none, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(row_none, LV_OBJ_FLAG_HIDDEN);
    if (shown == 0) lv_obj_clear_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
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

static void ta_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) apply_filter();
    else if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) show_keyboard(true);
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) show_keyboard(false);
}

static void finish(uint16_t id, bool picked) {
    show_keyboard(false);
    if (picked && pick_cb) pick_cb(id, pick_ctx);
    ui_show(return_page);
}

static void row_cb(lv_event_t *e) {
    finish((uint16_t)(intptr_t)lv_event_get_user_data(e), true);
}

static void cancel_cb(lv_event_t *) {
    finish(PARAM_NONE, false);
}

static lv_obj_t *make_row(const char *name, const char *group, uint16_t id) {
    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_size(row, lv_pct(100), 46);
    lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_t *l = make_label(row, &lv_font_montserrat_20, COLOR_TEXT, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (group) {
        lv_obj_t *g = make_label(row, &lv_font_montserrat_16, COLOR_DIM, group);
        lv_obj_align(g, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

void picker_build(lv_obj_t *page) {
    search_ta = lv_textarea_create(page);
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_placeholder_text(search_ta, "Szukaj po nazwie lub grupie...");
    lv_obj_set_style_text_font(search_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_size(search_ta, 335, 46);
    lv_obj_set_pos(search_ta, 8, 5);
    lv_obj_add_event_cb(search_ta, ta_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *cancel = make_button(page, "Anuluj", cancel_cb, NULL);
    lv_obj_set_size(cancel, 120, 46);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - TOP_H);
    lv_obj_set_pos(list, 0, TOP_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    row_none = make_row("(brak)", NULL, PARAM_NONE);
    for (int id = 0; id < P_COUNT; id++) {
        rows[id] = make_row(PARAM_INFO[id].name, GROUP_NAMES[PARAM_INFO[id].group], id);
    }
    lbl_empty = make_label(list, &lv_font_montserrat_20, COLOR_DIM, "Nic nie znaleziono.");

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
    lv_textarea_set_text(search_ta, "");
    apply_filter();
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    ui_show(PAGE_PICKER);
}
