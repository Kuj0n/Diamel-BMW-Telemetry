#include "ui.h"
#include "ui_internal.h"
#include "catalog.h"
#include "clock.h"
#include "comm.h"
#include <Arduino.h>
#include "display.h"
#include "logger.h"
#include "power.h"
#include "settings.h"
#include "wifi_web.h"

extern const lv_img_dsc_t diamel_logo;
extern const lv_img_dsc_t bmw_logo;

#define UI_REFRESH_MS 100
#define SWIPE_PAGES 3 // Odczyty 1-3 - kolejne kafelki tileview

static const char *PAGE_TITLES[PAGE_COUNT] = {
    "Menu", "Odczyty", "Odczyty", "Odczyty", "Wykres", "Bledy", "Pliki", "Wi-Fi", "Ustawienia", "Wybor odczytow",
    "Wybierz wartosc", "Informacje", "Adapter BLE", "Siec domowa"};

static lv_obj_t *page_obj[PAGE_COUNT]; // kontener strony (dla stron przesuwanych - kafelek)
static lv_obj_t *tileview;
static lv_obj_t *lbl_title;
static lv_obj_t *btn_rec;
static lv_obj_t *lbl_rec;
static lv_obj_t *lbl_wifi;
static lv_obj_t *lbl_source; // DEMO / CAN
static lv_obj_t *lbl_perf;   // FPS i CPU w rogu ekranu (włączane w ustawieniach)
static lv_obj_t *lbl_clock;  // godzina na pasku statusu
static lv_obj_t *banner;     // komunikat o stanie połączenia z autem
static lv_obj_t *toast;
static lv_timer_t *toast_timer;
static Page active_page = PAGE_MENU;

// --- Pomocnicze ---
lv_obj_t *plain_cont(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, COLOR_BTN, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = make_label(b, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_center(l);
    return b;
}

// Ustawienie tekstu przerysowuje etykietę - pomijamy, gdy treść się nie zmieniła
void set_text_if_changed(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void format_value(char *buf, size_t len, ParamIdx id) {
    float v;
    if (id >= params_count() || !telemetry_value(id, v)) {
        snprintf(buf, len, "--");
        return;
    }
    const char *text = param_value_text(id, v);
    if (text) snprintf(buf, len, "%s", text);
    else snprintf(buf, len, "%.*f", param(id).decimals, v);
}

lv_color_t module_color(int module) {
    static const uint32_t COLORS[] = {0x64B5F6, 0x80CBC4, 0xFFB74D, 0xBA68C8, 0xAED581, 0xF06292, 0x4DD0E1, 0xFFD54F};
    if (module == MODULE_DEMO) return COLOR_DIM;
    return lv_color_hex(COLORS[(module - 1) % (sizeof(COLORS) / sizeof(COLORS[0]))]);
}

static bool is_swipe_page(Page p) {
    return p >= PAGE_LIVE && p <= PAGE_LIVE3;
}

// --- Nawigacja ---
static void set_title(Page p) {
    if (is_swipe_page(p)) {
        lv_label_set_text_fmt(lbl_title, "%s  %d/%d", PAGE_TITLES[p], p - PAGE_LIVE + 1, SWIPE_PAGES);
    } else {
        lv_label_set_text(lbl_title, PAGE_TITLES[p]);
    }
}

void ui_show(Page page) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!is_swipe_page((Page)i)) lv_obj_add_flag(page_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (is_swipe_page(page)) {
        lv_obj_clear_flag(tileview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_tile(tileview, page_obj[page], LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(tileview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(page_obj[page], LV_OBJ_FLAG_HIDDEN);
    }
    active_page = page;
    set_title(page);
    if (page == PAGE_FILES) files_page_open();
    if (page == PAGE_INFO) info_page_open();
    if (page == PAGE_WIFI) wifi_page_refresh();
    if (page == PAGE_BLE) ble_page_open();
    if (page == PAGE_WIFI_HOME) wifi_home_page_open();
}

// --- Krótki komunikat na dole ekranu (znika 2.5 s po ostatnim komunikacie) ---
static void toast_hide_cb(lv_timer_t *) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = NULL; // jednorazowy timer LVGL usuwa się sam
}

void ui_toast(const char *text) {
    lv_label_set_text(lv_obj_get_child(toast, 0), text);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) {
        lv_timer_reset(toast_timer);
    } else {
        toast_timer = lv_timer_create(toast_hide_cb, 2500, NULL);
        lv_timer_set_repeat_count(toast_timer, 1);
    }
}

// --- Okno wpisywania liczby (klawiatura numeryczna nad całym ekranem) ---
static lv_obj_t *num_overlay;
static number_cb_t num_cb;

static void num_close_async(void *) {
    if (num_overlay) lv_obj_del(num_overlay);
    num_overlay = NULL;
}

static void num_kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
    if (code == LV_EVENT_READY) {
        lv_obj_t *ta = lv_keyboard_get_textarea(lv_event_get_target(e));
        char text[24];
        strlcpy(text, lv_textarea_get_text(ta), sizeof(text));
        for (char *p = text; *p; p++) {
            if (*p == ',') *p = '.';
        }
        char *end;
        double v = strtod(text, &end);
        if (end != text && num_cb) num_cb(v);
    }
    lv_async_call(num_close_async, NULL); // klawiatura usuwana po obsłudze jej zdarzenia
}

void ui_number_input(const char *title, double initial, uint8_t decimals, number_cb_t cb) {
    if (num_overlay) return;
    num_cb = cb;
    num_overlay = plain_cont(lv_layer_top());
    lv_obj_set_size(num_overlay, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(num_overlay, LV_OBJ_FLAG_CLICKABLE); // dotyk nie przechodzi do strony pod spodem
    lv_obj_set_style_bg_color(num_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(num_overlay, LV_OPA_90, 0);

    lv_obj_t *l = make_label(num_overlay, &lv_font_montserrat_20, COLOR_TEXT, title);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *ta = lv_textarea_create(num_overlay);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789.,-");
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_32, 0);
    lv_obj_set_width(ta, 300);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 80);
    char buf[24];
    snprintf(buf, sizeof(buf), "%.*f", decimals, initial);
    lv_textarea_set_text(ta, buf);

    lv_obj_t *kb = lv_keyboard_create(num_overlay);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(kb, SCREEN_W, 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, num_kb_cb, LV_EVENT_ALL, NULL);
}

static void build_toast() {
    toast = plain_cont(lv_layer_top());
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x323232), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    make_label(toast, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

static void tileview_cb(lv_event_t *e) {
    lv_obj_t *act = lv_tileview_get_tile_act(lv_event_get_target(e));
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (is_swipe_page((Page)i) && page_obj[i] == act) {
            active_page = (Page)i;
            set_title(active_page);
        }
    }
}

// --- Pasek statusu (warstwa top - widoczny na każdej stronie) ---
static void home_cb(lv_event_t *) {
    ui_show(PAGE_MENU);
}

static void rec_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(btn_rec, LV_STATE_CHECKED);
    if (!on) {
        logger_stop();
    } else if (!logger_start()) {
        lv_obj_clear_state(btn_rec, LV_STATE_CHECKED);
        ui_toast("Brak karty SD");
    }
}

// Czas nagrania na przycisku REC; wyłączenie przycisku po błędzie zapisu
static void rec_refresh() {
    if (logger_take_error()) {
        lv_obj_clear_state(btn_rec, LV_STATE_CHECKED);
        ui_toast("Blad zapisu na karte SD");
    }
    char buf[16];
    if (logger_active()) {
        uint32_t s = logger_elapsed_ms() / 1000;
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    } else {
        snprintf(buf, sizeof(buf), "REC");
    }
    set_text_if_changed(lbl_rec, buf);
}

static void build_status_bar() {
    lv_obj_t *bar = plain_cont(lv_layer_top());
    lv_obj_set_size(bar, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_style_bg_color(bar, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_LINE, 0);

    lv_obj_t *home = make_button(bar, LV_SYMBOL_HOME, home_cb, NULL);
    lv_obj_set_size(home, 50, 32);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 6, 0);

    lbl_title = make_label(bar, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 66, 0);

    // Źródło danych: DEMO na pomarańczowo (wartości symulowane), auto na szaro
    lbl_source = make_label(bar, &lv_font_montserrat_16, COLOR_ACCENT, "");
    lv_obj_align(lbl_source, LV_ALIGN_RIGHT_MID, -140, 0);

    // Godzina (gdy zegar ustawiony)
    lbl_clock = make_label(bar, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, -205, 0);

    lbl_wifi = make_label(bar, &lv_font_montserrat_20, COLOR_ACCENT, LV_SYMBOL_WIFI);
    lv_obj_align(lbl_wifi, LV_ALIGN_RIGHT_MID, -106, 0);
    lv_obj_add_flag(lbl_wifi, LV_OBJ_FLAG_HIDDEN);

    // REC domyślnie wyłączony przy każdym starcie
    btn_rec = lv_btn_create(bar);
    lv_obj_set_size(btn_rec, 90, 30);
    lv_obj_align(btn_rec, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(btn_rec, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(btn_rec, COLOR_BTN, 0);
    lv_obj_set_style_bg_color(btn_rec, COLOR_REC_ON, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn_rec, rec_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_rec = make_label(btn_rec, &lv_font_montserrat_16, COLOR_TEXT, "REC");
    lv_obj_center(lbl_rec);
}

// --- Menu główne ---
struct MenuItem {
    const char *symbol;
    const char *text;
    Page page;
};

static const MenuItem MENU_ITEMS[] = {
    {LV_SYMBOL_LIST,     "Odczyty",    PAGE_LIVE},
    {LV_SYMBOL_IMAGE,    "Wykres",     PAGE_CHART},
    {LV_SYMBOL_WARNING,  "Bledy",      PAGE_DTC},
    {LV_SYMBOL_SD_CARD,  "Pliki",      PAGE_FILES},
    {LV_SYMBOL_WIFI,     "Wi-Fi",      PAGE_WIFI},
    {LV_SYMBOL_SETTINGS, "Ustawienia", PAGE_SETTINGS},
    {LV_SYMBOL_EYE_OPEN, "Info",       PAGE_INFO},
    {LV_SYMBOL_PLAY,     "Start",      PAGE_COUNT}, // PAGE_COUNT = przełącznik komunikacji z autem
};

static lv_obj_t *comm_tile;
static lv_obj_t *comm_tile_symbol;
static lv_obj_t *comm_tile_text;

static void comm_toggle() {
    if (comm_running()) {
        comm_stop();
        ui_toast("Komunikacja zatrzymana");
    } else if (g_settings.data_source == SRC_DEMO) {
        ui_toast("Tryb DEMO - wybierz CAN w Ustawieniach");
    } else {
        comm_start();
        ui_toast("Komunikacja uruchomiona");
    }
}

static void menu_cb(lv_event_t *e) {
    Page p = (Page)(intptr_t)lv_event_get_user_data(e);
    if (p == PAGE_COUNT) comm_toggle();
    else ui_show(p);
}

static void comm_tile_refresh() {
    bool on = comm_running();
    set_text_if_changed(comm_tile_symbol, on ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    set_text_if_changed(comm_tile_text, on ? "Stop" : "Start");
    lv_color_t c = !on ? COLOR_PANEL : comm_state() == COMM_ERROR ? lv_color_hex(0x5D1F1F) : lv_color_hex(0x1B5E20);
    lv_obj_set_style_bg_color(comm_tile, c, 0);
}

static void build_menu(lv_obj_t *page) {
    const int cols = 3, w = 146, h = 128, gap = 10;
    const int x0 = (SCREEN_W - cols * w - (cols - 1) * gap) / 2;
    int i = 0;
    for (const MenuItem &m : MENU_ITEMS) {
        lv_obj_t *b = lv_btn_create(page);
        lv_obj_set_size(b, w, h);
        lv_obj_set_pos(b, x0 + (i % cols) * (w + gap), 10 + (i / cols) * (h + gap));
        lv_obj_set_style_bg_color(b, COLOR_PANEL, 0);
        lv_obj_set_style_border_color(b, COLOR_LINE, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(b, menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m.page);
        lv_obj_t *sym = make_label(b, &lv_font_montserrat_32, COLOR_ACCENT, m.symbol);
        lv_obj_t *l = make_label(b, &lv_font_montserrat_20, COLOR_TEXT, m.text);
        lv_obj_set_style_pad_top(l, 8, 0);
        if (m.page == PAGE_COUNT) {
            comm_tile = b;
            comm_tile_symbol = sym;
            comm_tile_text = l;
        }
        i++;
    }
}

// --- Odświeżanie danych na ekranie ---
// Zmiany z przeglądarki: przebudowa stron, które z nich korzystają
static void apply_web_settings() {
    uint16_t old_chart[CHART_SERIES_MAX];
    memcpy(old_chart, g_settings.chart_series, sizeof(old_chart));
    if (!wifi_apply_web_settings()) return;
    live_rebuild_all();
    live_editor_open();
    if (memcmp(old_chart, g_settings.chart_series, sizeof(old_chart)) != 0) chart_rebuild();
    settings_page_sync();
    ui_toast("Ustawienia zmienione z przegladarki");
}

// Pasek statusu: źródło danych; róg ekranu: FPS / CPU (co 0.5 s)
// Pasek pod paskiem statusu na stronach z danymi: komunikacja zatrzymana / błąd połączenia

static void build_banner() {
    banner = make_label(lv_layer_top(), &lv_font_montserrat_16, COLOR_TEXT, "");
    lv_obj_set_width(banner, SCREEN_W);
    lv_label_set_long_mode(banner, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x5D1F1F), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(banner, 4, 0);
    lv_obj_set_pos(banner, 0, STATUS_BAR_H);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

static void banner_refresh() {
    bool data_page = is_swipe_page(active_page) || active_page == PAGE_CHART;
    CommState st = comm_state();
    if (!data_page || g_settings.data_source == SRC_DEMO || (comm_running() && st == COMM_RUNNING)) {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[100];
    if (!comm_running()) snprintf(buf, sizeof(buf), "Komunikacja zatrzymana - Menu > Start");
    else comm_status_text(buf, sizeof(buf));
    set_text_if_changed(banner, buf);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

static void status_refresh() {
    set_text_if_changed(lbl_source, data_source_name(g_settings.data_source));
    // DEMO pomarańczowo; auto: szaro = zatrzymana, zielono = odpowiada, czerwono = błąd połączenia
    lv_color_t c = COLOR_ACCENT;
    if (g_settings.data_source != SRC_DEMO) {
        c = !comm_running() ? COLOR_DIM : comm_state() == COMM_ERROR ? COLOR_REC_ON : lv_color_hex(0x66BB6A);
    }
    lv_obj_set_style_text_color(lbl_source, c, 0);
    if (wifi_active()) lv_obj_clear_flag(lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    banner_refresh();
    comm_tile_refresh();

    static uint8_t tick = 0;
    if (++tick < 5) return;
    tick = 0;
    struct tm tm;
    if (clock_now(tm)) {
        char t[8];
        strftime(t, sizeof(t), "%H:%M", &tm);
        set_text_if_changed(lbl_clock, t);
    } else {
        set_text_if_changed(lbl_clock, "");
    }
    uint16_t fps = display_fps(); // wołane zawsze, żeby pomiar dotyczył ostatnich 0.5 s
    if (!g_settings.show_fps && !g_settings.show_cpu) {
        lv_obj_add_flag(lbl_perf, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[32] = "";
    if (g_settings.show_fps) snprintf(buf, sizeof(buf), "%u FPS", fps);
    if (g_settings.show_cpu) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "%s%u%% CPU", n ? "  " : "", (unsigned)(100 - lv_timer_get_idle()));
    }
    set_text_if_changed(lbl_perf, buf);
    lv_obj_clear_flag(lbl_perf, LV_OBJ_FLAG_HIDDEN);
}

static void build_perf_label() {
    lbl_perf = make_label(lv_layer_top(), &lv_font_montserrat_16, COLOR_TEXT, "");
    lv_obj_set_style_bg_color(lbl_perf, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_perf, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(lbl_perf, 4, 0);
    lv_obj_align(lbl_perf, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_add_flag(lbl_perf, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_cb(lv_timer_t *) {
    apply_web_settings();


    // Wykres zbiera próbki zawsze, żeby historia była ciągła po przejściu na stronę
    chart_sample();

    // Etykiety aktualizujemy tylko na widocznej stronie
    switch (active_page) {
        case PAGE_LIVE:
        case PAGE_LIVE2:
        case PAGE_LIVE3:    live_refresh(active_page - PAGE_LIVE); break;
        case PAGE_CHART:    chart_refresh_labels(); break;
        case PAGE_INFO:     info_page_refresh(); break;
        case PAGE_WIFI:     wifi_page_refresh(); break;
        case PAGE_DTC:      dtc_page_refresh(); break;
        case PAGE_SETTINGS: settings_page_refresh(); break;
        case PAGE_BLE:      ble_page_refresh(); break;
        case PAGE_WIFI_HOME: wifi_home_page_refresh(); break;
        default: break;
    }
    rec_refresh();
    status_refresh();
    wifi_overlay_tick();
    wifi_tick();
    files_format_tick();
    power_tick();
    settings_tick();
}

static lv_obj_t *make_page(lv_obj_t *scr) {
    lv_obj_t *p = plain_cont(scr);
    lv_obj_set_size(p, SCREEN_W, PAGE_H);
    lv_obj_set_pos(p, 0, STATUS_BAR_H);
    return p;
}

static void build_main() {
    // Strony korzystają z katalogu parametrów - czekamy na jego wczytanie (zwykle kończy się w trakcie logo)
    if (!catalog_wait(20000)) Serial.println("Katalog: przekroczony czas wczytywania");
    settings_resolve_params();
    comm_init(); // odpytywanie auta (autostart wg ustawień)

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Strony przesuwane palcem
    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, SCREEN_W, PAGE_H);
    lv_obj_set_pos(tileview, 0, STATUS_BAR_H);
    lv_obj_set_style_bg_color(tileview, COLOR_BG, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < SWIPE_PAGES; i++) {
        lv_dir_t dir = LV_DIR_NONE;
        if (i > 0) dir |= LV_DIR_LEFT;
        if (i < SWIPE_PAGES - 1) dir |= LV_DIR_RIGHT;
        page_obj[PAGE_LIVE + i] = lv_tileview_add_tile(tileview, i, 0, dir);
    }
    lv_obj_add_event_cb(tileview, tileview_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Pozostałe strony
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!is_swipe_page((Page)i)) page_obj[i] = make_page(scr);
    }

    build_menu(page_obj[PAGE_MENU]);
    for (int i = 0; i < SWIPE_PAGES; i++) live_build(i, page_obj[PAGE_LIVE + i]);
    chart_build(page_obj[PAGE_CHART]);
    dtc_page_build(page_obj[PAGE_DTC]);
    files_page_build(page_obj[PAGE_FILES]);
    wifi_page_build(page_obj[PAGE_WIFI]);
    settings_page_build(page_obj[PAGE_SETTINGS]);
    live_editor_build(page_obj[PAGE_LIVE_EDIT]);
    picker_build(page_obj[PAGE_PICKER]);
    info_page_build(page_obj[PAGE_INFO]);
    ble_page_build(page_obj[PAGE_BLE]);
    wifi_home_page_build(page_obj[PAGE_WIFI_HOME]);
    build_status_bar();
    build_toast();
    build_perf_label();
    build_banner();

    static const Page START_PAGES[START_COUNT] = {PAGE_MENU, PAGE_LIVE, PAGE_CHART, PAGE_DTC};
    ui_show(START_PAGES[g_settings.start_screen]);

    // Przełączenie bez animacji LVGL - przejście robi podświetlenie (patrz splash_end)
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(scr);
    lv_obj_del(old);
    lv_timer_create(refresh_cb, UI_REFRESH_MS, NULL);
    // Pierwsza pełna klatka rysowana przy zgaszonym podświetleniu - ekran rozjaśnia się dopiero po niej
    lv_refr_now(NULL);

    Serial.printf("Interfejs gotowy: wolny RAM wewn. %u KB (najwiekszy blok %u KB), PSRAM %u KB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024));
}

// --- Ekran powitalny: logo BMW M, potem logo Diamel ---
// Loga (po 418 KB we flashu) rozjaśniamy podświetleniem, a nie przezroczystością:
// animacja opa przerysowuje całe logo w każdej klatce, co zapycha szynę flash/PSRAM
// (wspólną na ESP32-S3) i psuje obraz.
static lv_obj_t *splash_bmw;
static lv_obj_t *splash_diamel;

static void anim_backlight_cb(void *, int32_t v) {
    display_set_brightness((uint8_t)v);
}

static void backlight_fade(int32_t from, int32_t to, uint32_t ms, lv_anim_ready_cb_t ready_cb) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &g_settings.brightness);
    lv_anim_set_exec_cb(&a, anim_backlight_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static void splash_dark(lv_anim_t *) {
    build_main();
    backlight_fade(0, g_settings.brightness, 300, NULL);
}

// --- Ekran ładowania katalogu (po logo) ---
// Katalog czyta się dopiero tutaj: wczytywanie w trakcie logo (duże obrazy z flasha + zapis do PSRAM)
// przeciążało wspólną szynę pamięci i obraz przeskakiwał.
static lv_obj_t *load_bar;
static lv_obj_t *load_label;

static void loading_done(lv_anim_t *) {
    splash_dark(nullptr);
}

static void loading_tick(lv_timer_t *t) {
    int p = catalog_progress();
    lv_bar_set_value(load_bar, p, LV_ANIM_OFF);
    lv_label_set_text_fmt(load_label, "Wczytywanie katalogu parametrow... %d%%", p);
    if (!catalog_wait(0)) return;
    lv_timer_del(t);
    backlight_fade(g_settings.brightness, 0, 200, loading_done);
}

static void loading_start(lv_anim_t *) {
    lv_obj_add_flag(splash_diamel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *scr = lv_scr_act();
    load_label = make_label(scr, &lv_font_montserrat_20, COLOR_TEXT, "Wczytywanie katalogu parametrow...");
    lv_obj_align(load_label, LV_ALIGN_CENTER, 0, -30);
    load_bar = lv_bar_create(scr);
    lv_obj_set_size(load_bar, 360, 16);
    lv_obj_set_style_bg_color(load_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_align(load_bar, LV_ALIGN_CENTER, 0, 10);
    lv_obj_t *hint = make_label(scr, &lv_font_montserrat_16, COLOR_DIM, "karta SD: /diamel");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 45);
    catalog_begin();
    backlight_fade(0, g_settings.brightness, 200, NULL);
    lv_timer_create(loading_tick, 100, NULL);
}

static void splash_end(lv_timer_t *) {
    backlight_fade(g_settings.brightness, 0, 300, loading_start);
}

static void splash_bmw_dark(lv_anim_t *) {
    lv_obj_add_flag(splash_bmw, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(splash_diamel, LV_OBJ_FLAG_HIDDEN);
    backlight_fade(0, g_settings.brightness, 700, NULL);
}

static void splash_bmw_end(lv_timer_t *) {
    backlight_fade(g_settings.brightness, 0, 400, splash_bmw_dark);
}

static lv_obj_t *splash_image(lv_obj_t *scr, const lv_img_dsc_t *src) {
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, src);
    lv_obj_center(img);
    return img;
}

void ui_init() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);

    // Oś czasu: BMW rozjaśnia się (0-0.6 s), gaśnie (1.6-2.0 s), Diamel rozjaśnia się (2.0-2.7 s),
    // od 4.3 s ekran ładowania katalogu z paskiem postępu, potem strony
    splash_bmw = splash_image(scr, &bmw_logo);
    splash_diamel = splash_image(scr, &diamel_logo);
    lv_obj_add_flag(splash_diamel, LV_OBJ_FLAG_HIDDEN);
    display_set_brightness(0);
    backlight_fade(0, g_settings.brightness, 600, NULL);

    lv_timer_t *t = lv_timer_create(splash_bmw_end, 1600, NULL);
    lv_timer_set_repeat_count(t, 1);
    t = lv_timer_create(splash_end, 4300, NULL);
    lv_timer_set_repeat_count(t, 1);
}
