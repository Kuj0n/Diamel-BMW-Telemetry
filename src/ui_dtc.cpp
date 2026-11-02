#include "ui_internal.h"
#include "psram.h"
#include <Arduino.h>
#include "dtc.h"

// --- Strona Bledy: odczyt kodów błędów tylko po naciśnięciu przycisku, z opisami z katalogu ---
static lv_obj_t *dd_module;
static lv_obj_t *btn_read;
static lv_obj_t *btn_export;
static lv_obj_t *lbl_status;
static lv_obj_t *list;
static int dd_modules[64]; // pozycja listy (bez "Wszystkie") -> moduł
static DtcState prev_state = DTC_IDLE;
static lv_obj_t *armed_btn;   // przycisk "Kasuj" czekający na potwierdzenie
static uint32_t armed_ms;

// Pierwsze dotknięcie: "Na pewno?", drugie w ciągu 4 s kasuje błędy tego modułu
static void clear_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int module = (int)(intptr_t)lv_event_get_user_data(e);
    if (armed_btn != btn || millis() - armed_ms > 4000) {
        if (armed_btn) lv_label_set_text(lv_obj_get_child(armed_btn, 0), LV_SYMBOL_TRASH " Kasuj");
        armed_btn = btn;
        armed_ms = millis();
        lv_label_set_text(lv_obj_get_child(btn, 0), "Na pewno?");
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB71C1C), 0);
        return;
    }
    armed_btn = NULL;
    dtc_clear(module);
}

static void read_cb(lv_event_t *) {
    uint16_t sel = lv_dropdown_get_selected(dd_module);
    dtc_read(sel == 0 ? -1 : dd_modules[sel - 1]);
}

static void export_cb(lv_event_t *) {
    char name[32], msg[64];
    if (dtc_export(name, sizeof(name))) snprintf(msg, sizeof(msg), "Zapisano: %s (Pliki)", name);
    else snprintf(msg, sizeof(msg), "Nie zapisano - brak karty SD?");
    ui_toast(msg);
}

static void results_rebuild() {
    armed_btn = NULL;
    lv_obj_clean(list);
    if (dtc_state() != DTC_DONE) return;
    int n = dtc_count();
    if (n == 0) {
        make_label(list, &lv_font_montserrat_20, lv_color_hex(0x81C784), "Brak zapisanych bledow.");
        return;
    }
    int last_module = -1;
    for (int i = 0; i < n; i++) {
        const DtcEntry &e = dtc_entry(i);
        if (e.module != last_module) {
            last_module = e.module;
            lv_obj_t *head = plain_cont(list);
            lv_obj_set_size(head, lv_pct(100), 44);
            lv_obj_set_style_pad_top(head, 6, 0);
            lv_obj_t *h = make_label(head, &lv_font_montserrat_16, module_color(e.module), module_def(e.module).name);
            lv_obj_align(h, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_t *del = make_button(head, LV_SYMBOL_TRASH " Kasuj", clear_cb, (void *)(intptr_t)e.module);
            lv_obj_set_size(del, 130, 38);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_obj_t *row = plain_cont(list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        bool now = e.status & 0x01; // błąd występuje teraz
        lv_obj_t *code = make_label(row, &lv_font_montserrat_20, now ? COLOR_REC_ON : COLOR_ACCENT, "");
        lv_label_set_text_fmt(code, "%06lX  %s", (unsigned long)e.code, now ? "aktywny" : "zapisany");
        lv_obj_t *t = make_label(row, &lv_font_montserrat_16, COLOR_TEXT, e.text);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, lv_pct(100));
    }
}

void dtc_page_refresh() {
    DtcState st = dtc_state();
    char buf[120];
    dtc_status_text(buf, sizeof(buf));
    set_text_if_changed(lbl_status, buf);
    lv_color_t c = st == DTC_FAILED ? COLOR_REC_ON : dtc_simulated() ? COLOR_ACCENT : COLOR_DIM;
    lv_obj_set_style_text_color(lbl_status, c, 0);
    if (st == DTC_READING) lv_obj_add_state(btn_read, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn_read, LV_STATE_DISABLED);
    if (st == DTC_DONE) lv_obj_clear_state(btn_export, LV_STATE_DISABLED);
    else lv_obj_add_state(btn_export, LV_STATE_DISABLED);
    if (st != prev_state && st != DTC_READING) results_rebuild();
    prev_state = st;
}

void dtc_page_build(lv_obj_t *page) {
    // Lista modułów: tylko te, dla których katalog ma opisy błędów
    const size_t OPT_LEN = 1600;
    char *options = psram_alloc<char>(OPT_LEN);
    if (!options) return;
    size_t o = snprintf(options, OPT_LEN, "Wszystkie moduly");
    int k = 0;
    for (int m = 1; m < modules_count() && k < 64 && o < OPT_LEN - 40; m++) {
        if (!module_def(m).dtc_count) continue;
        o += snprintf(options + o, OPT_LEN - o, "\n%s", module_def(m).name);
        dd_modules[k++] = m;
    }
    dd_module = lv_dropdown_create(page);
    lv_dropdown_set_options(dd_module, options); // LVGL kopiuje tekst
    free(options);
    lv_obj_set_style_text_font(dd_module, &lv_font_montserrat_16, 0);
    lv_obj_set_size(dd_module, 220, 44);
    lv_obj_set_pos(dd_module, 8, 6);

    btn_read = make_button(page, LV_SYMBOL_REFRESH " Odczytaj", read_cb, NULL);
    lv_obj_set_size(btn_read, 140, 44);
    lv_obj_set_pos(btn_read, 236, 6);

    btn_export = make_button(page, LV_SYMBOL_SAVE, export_cb, NULL);
    lv_obj_set_size(btn_export, 88, 44);
    lv_obj_align(btn_export, LV_ALIGN_TOP_RIGHT, -8, 6);

    lbl_status = make_label(page, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_width(lbl_status, SCREEN_W - 24);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_status, 12, 56);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - 100);
    lv_obj_set_pos(list, 0, 100);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *hint = make_label(list, &lv_font_montserrat_16, COLOR_FAINT,
                                "Odczyt tylko po nacisnieciu przycisku. " LV_SYMBOL_SAVE " zapisuje wynik do pliku "
                                "(Pliki / WWW). Kasowanie: przycisk przy module (dwa dotkniecia) - tylko ten modul.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    dtc_page_refresh();
}
