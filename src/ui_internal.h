#pragma once
// Wspólne elementy interfejsu - używane tylko przez pliki ui_*.cpp
#include <lvgl.h>
#include "config.h"
#include "telemetry.h"

#define STATUS_BAR_H 40
#define PAGE_H (SCREEN_H - STATUS_BAR_H)

#define COLOR_BG       lv_color_black()
#define COLOR_PANEL    lv_color_hex(0x101010)
#define COLOR_LINE     lv_color_hex(0x303030)
#define COLOR_TEXT     lv_color_white()
#define COLOR_DIM      lv_color_hex(0x9E9E9E)
#define COLOR_FAINT    lv_color_hex(0x616161)
#define COLOR_ACCENT   lv_color_hex(0xFF9800)
#define COLOR_REC_ON   lv_color_hex(0xD32F2F)
#define COLOR_BTN      lv_color_hex(0x424242)

enum Page : uint8_t {
    PAGE_MENU,
    PAGE_LIVE,      // strony przesuwane palcem: LIVE, CHART, DTC
    PAGE_CHART,
    PAGE_DTC,
    PAGE_FILES,
    PAGE_WIFI,
    PAGE_SETTINGS,
    PAGE_LIVE_EDIT,
    PAGE_PICKER,
    PAGE_INFO,
    PAGE_COUNT
};

void ui_show(Page page);
void ui_toast(const char *text); // krótki komunikat na dole ekranu

// Pliki na karcie SD (ui_files.cpp)
void files_page_build(lv_obj_t *page);
void files_page_open();

// Pomocnicze
lv_obj_t *plain_cont(lv_obj_t *parent);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data);
void set_text_if_changed(lv_obj_t *label, const char *text);
void format_value(char *buf, size_t len, ParamId id, const TelemetrySnapshot &s);

// Strona Odczyty i edytor wyboru/kolejności (ui_live.cpp)
void live_build(lv_obj_t *page);
void live_rebuild();
void live_refresh(const TelemetrySnapshot &s);
void live_editor_build(lv_obj_t *page);
void live_editor_open();

// Wykres (ui_chart.cpp)
void chart_build(lv_obj_t *page);
void chart_rebuild();
void chart_sample(const TelemetrySnapshot &s);
void chart_refresh_labels(const TelemetrySnapshot &s);

// Wyszukiwarka parametrów (ui_picker.cpp)
// Po wyborze wywołuje cb(id, ctx) i wraca na return_page; id = PARAM_NONE dla pozycji "brak".
typedef void (*picker_cb_t)(uint16_t id, void *ctx);
typedef bool (*picker_exclude_t)(uint16_t id); // true = nie pokazuj (np. już wybrane)
void picker_build(lv_obj_t *page);
void picker_open(bool allow_none, picker_exclude_t exclude, picker_cb_t cb, void *ctx, Page return_page);

// Ustawienia (ui_settings.cpp)
void settings_page_build(lv_obj_t *page);

// Info: wersja, sprzęt, test CAN (ui_info.cpp)
void info_page_build(lv_obj_t *page);
void info_page_open();
void info_page_refresh();
