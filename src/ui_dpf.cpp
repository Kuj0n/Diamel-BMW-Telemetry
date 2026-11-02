#include "ui_internal.h"
#include <Arduino.h>
#include "comm.h"
#include "telemetry.h"

// --- Kafelek DPF: stały zestaw pomiarów filtra cząstek stałych z DDE (tylko odczyt) ---
// Parametry wg klucza z katalogu (adres 12 = DDE); brak w katalogu -> pozycja pominięta.
struct DpfItem {
    const char *key;
    const char *label; // krótka nazwa na kafelku
};

static const DpfItem ITEMS[] = {
    {"12:russmasse_im_partikelfilter_wert", "Sadza (pomiar)"},
    {"12:partikelmasse_simuliert_wert", "Sadza (model)"},
    {"12:oelaschenmasse_wert", "Popiol"},
    {"12:differenzdruck_ueber_partikelfilter_wert", "Roznica cisnien"},
    {"12:abgastemperatur_vor_partikelfilter_1_wert", "Temp. przed DPF"},
    {"12:regeneration_blockierung_und_freigabe_wert", "Blokada regeneracji"},
    {"12:strecke_seit_erfolgreicher_regeneration_wert", "Od regeneracji"},
    {"12:pfltrgn_tisncergn_wert", "Czas od regeneracji"},
    {"12:durchschnitt_kilometer_zwischen_regeneration", "Sredni odstep"},
    {"12:pfltrgn_ctrgnsuceep_mp_wert", "Liczba regeneracji"},
    {"12:pfltrgn_numrgn_wert", "Zadanie regeneracji"},
    {"12:status_regeneration_unterbrochen_wert", "Regeneracja przerwana"},
};
#define DPF_N (sizeof(ITEMS) / sizeof(ITEMS[0]))

static uint16_t ids[DPF_N];
static int count = 0;
static lv_obj_t *page_obj;
static lv_obj_t *lbl_value[DPF_N];
static bool built = false;

static void build_cells() {
    lv_obj_clean(page_obj);
    count = 0;
    for (const DpfItem &it : ITEMS) {
        ParamIdx id = param_by_name_key(it.key);
        if (id == PARAM_NONE) continue;
        int pos = count;
        ids[count++] = id;
        lv_obj_t *cell = plain_cont(page_obj);
        lv_obj_set_size(cell, 220, 70);
        lv_obj_set_pos(cell, 20 + (pos % 2) * 235, 4 + (pos / 2) * 70);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        make_label(cell, &lv_font_montserrat_16, COLOR_ACCENT, it.label);
        lv_obj_t *row = plain_cont(cell);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lbl_value[pos] = make_label(row, &lv_font_montserrat_32, COLOR_TEXT, "--");
        // Czas w sekundach pokazujemy w godzinach
        const char *unit = strcmp(param(id).unit, "s") == 0 ? "h" : param(id).unit;
        lv_obj_t *u = make_label(row, &lv_font_montserrat_16, COLOR_DIM, unit);
        lv_obj_set_style_pad_left(u, 6, 0);
        lv_obj_set_style_pad_bottom(u, 5, 0);
    }
    if (!count) {
        lv_obj_t *l = make_label(page_obj, &lv_font_montserrat_20, COLOR_DIM,
                                 "Brak parametrow DPF w katalogu.\n\nWgraj katalog sterownika silnika\n(System > Katalog).");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
    built = true;
}

void dpf_page_build(lv_obj_t *page) {
    page_obj = page;
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

// Wywoływane przy wejściu na kafelek: katalog jest już wczytany, komunikacja dostaje listę
void dpf_page_open() {
    if (!built) build_cells();
    comm_set_view_list(ids, count);
}

void dpf_page_refresh() {
    char buf[24];
    for (int i = 0; i < count; i++) {
        float v;
        if (strcmp(param(ids[i]).unit, "s") == 0 && telemetry_value(ids[i], v)) {
            snprintf(buf, sizeof(buf), "%.1f", v / 3600.0f);
        } else {
            format_value(buf, sizeof(buf), ids[i]);
        }
        set_text_if_changed(lbl_value[i], buf);
    }
}
