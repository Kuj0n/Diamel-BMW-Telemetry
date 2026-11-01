#include "ui_internal.h"
#include <Arduino.h>
#include "settings.h"

// --- Wykres: do 3 serii wybranych w ustawieniach, automatyczna skala ---
// Serie z jednostką pierwszej serii idą na lewą oś, pozostałe na prawą.
// Wartości w wykresie są liczbami całkowitymi: wartość * 10^(liczba miejsc po przecinku osi).
#define CHART_POINTS 100      // 10 s historii przy próbkowaniu co 100 ms
#define AUTORANGE_EVERY 10    // przeliczanie skali co 1 s

static const lv_color_t SERIES_COLORS[CHART_SERIES_MAX] = {
    LV_COLOR_MAKE(0xFF, 0x98, 0x00), // pomarańczowy
    LV_COLOR_MAKE(0x00, 0xBC, 0xD4), // niebieski
    LV_COLOR_MAKE(0x8B, 0xC3, 0x4A), // zielony
};

static lv_obj_t *chart_page;
static lv_obj_t *chart;
static int series_count;
static lv_chart_series_t *ser[CHART_SERIES_MAX];
static uint16_t ser_param[CHART_SERIES_MAX];
static uint8_t ser_axis[CHART_SERIES_MAX]; // 0 = lewa oś, 1 = prawa
static lv_obj_t *lbl_legend[CHART_SERIES_MAX];
static int32_t axis_scale[2];
static uint8_t axis_decimals[2];
static bool axis_used[2];
static uint32_t sample_count;

static lv_chart_axis_t axis_id(int a) {
    return a == 0 ? LV_CHART_AXIS_PRIMARY_Y : LV_CHART_AXIS_SECONDARY_Y;
}

static void tick_label_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_TICK_LABEL)) return;
    if (!dsc->text) return;
    int a;
    if (dsc->id == LV_CHART_AXIS_PRIMARY_Y) a = 0;
    else if (dsc->id == LV_CHART_AXIS_SECONDARY_Y) a = 1;
    else return;

    int32_t v = dsc->value;
    if (axis_decimals[a] == 0) {
        snprintf(dsc->text, dsc->text_length, "%ld", (long)v);
    } else {
        int32_t av = v < 0 ? -v : v;
        snprintf(dsc->text, dsc->text_length, "%s%ld.%0*ld", v < 0 ? "-" : "",
                 (long)(av / axis_scale[a]), axis_decimals[a], (long)(av % axis_scale[a]));
    }
}

void chart_build(lv_obj_t *page) {
    chart_page = page;
    chart_rebuild();
}

void chart_rebuild() {
    lv_obj_clean(chart_page);
    chart = NULL;
    series_count = 0;
    sample_count = 0;
    memset(axis_used, 0, sizeof(axis_used));
    memset(axis_decimals, 0, sizeof(axis_decimals));

    const char *primary_unit = NULL;
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        uint16_t id = g_settings.chart_series[i];
        if (id == PARAM_NONE) continue;
        if (!primary_unit) primary_unit = PARAM_INFO[id].unit;
        int a = strcmp(PARAM_INFO[id].unit, primary_unit) == 0 ? 0 : 1;
        ser_param[series_count] = id;
        ser_axis[series_count] = a;
        axis_used[a] = true;
        if (PARAM_INFO[id].decimals > axis_decimals[a]) axis_decimals[a] = PARAM_INFO[id].decimals;
        series_count++;
    }

    if (series_count == 0) {
        lv_obj_t *l = make_label(chart_page, &lv_font_montserrat_20, COLOR_DIM,
                                 "Brak wybranych wartosci.\nUstawienia > Wykres.");
        lv_obj_center(l);
        return;
    }
    for (int a = 0; a < 2; a++) {
        axis_scale[a] = 1;
        for (int d = 0; d < axis_decimals[a]; d++) axis_scale[a] *= 10;
    }

    for (int k = 0; k < series_count; k++) {
        lbl_legend[k] = make_label(chart_page, &lv_font_montserrat_16, SERIES_COLORS[k], "");
        lv_obj_set_pos(lbl_legend[k], 20, 6 + k * 22);
    }

    const int top = 76;
    const int left = 60;
    const int right = axis_used[1] ? 60 : 15;
    chart = lv_chart_create(chart_page);
    lv_obj_set_size(chart, SCREEN_W - left - right, PAGE_H - top - 30);
    lv_obj_set_pos(chart, left, top);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart, 6, 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR); // bez kropek na punktach
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(chart, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(chart, COLOR_LINE, 0);
    lv_obj_set_style_line_color(chart, COLOR_LINE, LV_PART_MAIN);
    lv_obj_set_style_text_color(chart, COLOR_DIM, LV_PART_TICKS);
    lv_obj_add_event_cb(chart, tick_label_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    for (int a = 0; a < 2; a++) {
        if (!axis_used[a]) continue;
        lv_chart_set_axis_tick(chart, axis_id(a), 8, 4, 6, 2, true, 50);
        lv_chart_set_range(chart, axis_id(a), 0, 100);
    }
    for (int k = 0; k < series_count; k++) {
        ser[k] = lv_chart_add_series(chart, SERIES_COLORS[k], axis_id(ser_axis[k]));
        lv_chart_set_all_value(chart, ser[k], LV_CHART_POINT_NONE);
    }
}

// Zakres osi = min/max widocznych danych + 10% marginesu
static void autorange() {
    for (int a = 0; a < 2; a++) {
        if (!axis_used[a]) continue;
        bool found = false;
        int32_t lo = 0, hi = 0;
        for (int k = 0; k < series_count; k++) {
            if (ser_axis[k] != a) continue;
            lv_coord_t *pts = lv_chart_get_y_array(chart, ser[k]);
            for (int p = 0; p < CHART_POINTS; p++) {
                if (pts[p] == LV_CHART_POINT_NONE) continue;
                if (!found) { lo = hi = pts[p]; found = true; }
                if (pts[p] < lo) lo = pts[p];
                if (pts[p] > hi) hi = pts[p];
            }
        }
        if (!found) continue;
        int32_t margin = (hi - lo) / 10;
        if (margin < axis_scale[a]) margin = axis_scale[a]; // minimum: 1 jednostka
        lv_chart_set_range(chart, axis_id(a), lo - margin, hi + margin);
    }
}

void chart_sample(const TelemetrySnapshot &s) {
    if (!chart) return;
    for (int k = 0; k < series_count; k++) {
        uint16_t id = ser_param[k];
        lv_coord_t v = LV_CHART_POINT_NONE;
        if (s.valid[id]) {
            float scaled = s.value[id] * axis_scale[ser_axis[k]];
            v = (lv_coord_t)constrain(lroundf(scaled), -32000L, 32000L);
        }
        lv_chart_set_next_value(chart, ser[k], v);
    }
    if (sample_count++ % AUTORANGE_EVERY == 0) autorange();
}

void chart_refresh_labels(const TelemetrySnapshot &s) {
    if (!chart) return;
    char buf[24], line[64];
    for (int k = 0; k < series_count; k++) {
        uint16_t id = ser_param[k];
        format_value(buf, sizeof(buf), (ParamId)id, s);
        snprintf(line, sizeof(line), "%s: %s %s", PARAM_INFO[id].name, buf, PARAM_INFO[id].unit);
        set_text_if_changed(lbl_legend[k], line);
    }
}
