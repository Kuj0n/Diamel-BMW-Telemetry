#include "ui.h"
#include "ui_internal.h"
#include <Arduino.h>
#include "display.h"
#include "logger.h"
#include "settings.h"

extern const lv_img_dsc_t diamel_logo;
extern const lv_img_dsc_t bmw_logo;

#define UI_REFRESH_MS 100
#define SWIPE_PAGES 3 // LIVE, CHART, DTC - kolejne kafelki tileview

static const char *PAGE_TITLES[PAGE_COUNT] = {
    "Menu", "Odczyty", "Wykres", "Bledy", "Pliki", "Wi-Fi", "Ustawienia", "Wybor odczytow", "Wybierz wartosc",
    "Informacje"};

static lv_obj_t *page_obj[PAGE_COUNT]; // kontener strony (dla stron przesuwanych - kafelek)
static lv_obj_t *tileview;
static lv_obj_t *lbl_title;
static lv_obj_t *btn_rec;
static lv_obj_t *lbl_rec;
static lv_obj_t *toast;
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

void format_value(char *buf, size_t len, ParamId id, const TelemetrySnapshot &s) {
    if (!s.valid[id]) {
        snprintf(buf, len, "--");
        return;
    }
    snprintf(buf, len, "%.*f", PARAM_INFO[id].decimals, s.value[id]);
}

static bool is_swipe_page(Page p) {
    return p == PAGE_LIVE || p == PAGE_CHART || p == PAGE_DTC;
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
}

// --- Krótki komunikat na dole ekranu (znika po 2.5 s) ---
static void toast_hide_cb(lv_timer_t *) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

void ui_toast(const char *text) {
    lv_label_set_text(lv_obj_get_child(toast, 0), text);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(toast_hide_cb, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
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

#if DEMO_MODE
    lv_obj_t *demo = make_label(bar, &lv_font_montserrat_16, COLOR_ACCENT, "DEMO");
    lv_obj_align(demo, LV_ALIGN_RIGHT_MID, -110, 0);
#endif

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
};

static void menu_cb(lv_event_t *e) {
    ui_show((Page)(intptr_t)lv_event_get_user_data(e));
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
        make_label(b, &lv_font_montserrat_32, COLOR_ACCENT, m.symbol);
        lv_obj_t *l = make_label(b, &lv_font_montserrat_20, COLOR_TEXT, m.text);
        lv_obj_set_style_pad_top(l, 8, 0);
        i++;
    }
}

// Strony, które dojdą w kolejnych etapach
static void build_placeholder(lv_obj_t *page, const char *text) {
    lv_obj_t *l = make_label(page, &lv_font_montserrat_20, COLOR_DIM, text);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

// --- Odświeżanie danych na ekranie ---
static void refresh_cb(lv_timer_t *) {
    TelemetrySnapshot s;
    telemetry_get(s);

    // Wykres zbiera próbki zawsze, żeby historia była ciągła po przejściu na stronę
    chart_sample(s);

    // Etykiety aktualizujemy tylko na widocznej stronie
    switch (active_page) {
        case PAGE_LIVE:     live_refresh(s); break;
        case PAGE_CHART:    chart_refresh_labels(s); break;
        case PAGE_INFO:     info_page_refresh(); break;
        default: break;
    }
    rec_refresh();
    settings_tick();
}

static lv_obj_t *make_page(lv_obj_t *scr) {
    lv_obj_t *p = plain_cont(scr);
    lv_obj_set_size(p, SCREEN_W, PAGE_H);
    lv_obj_set_pos(p, 0, STATUS_BAR_H);
    return p;
}

static void build_main() {
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
    live_build(page_obj[PAGE_LIVE]);
    chart_build(page_obj[PAGE_CHART]);
    build_placeholder(page_obj[PAGE_DTC], "Odczyt bledow DDE / xDrive / IHKA\n- po podlaczeniu CAN.");
    files_page_build(page_obj[PAGE_FILES]);
    build_placeholder(page_obj[PAGE_WIFI], "Wi-Fi i serwer WWW\n- kolejny etap.");
    settings_page_build(page_obj[PAGE_SETTINGS]);
    live_editor_build(page_obj[PAGE_LIVE_EDIT]);
    picker_build(page_obj[PAGE_PICKER]);
    info_page_build(page_obj[PAGE_INFO]);
    build_status_bar();
    build_toast();

    static const Page START_PAGES[START_COUNT] = {PAGE_MENU, PAGE_LIVE, PAGE_CHART, PAGE_DTC};
    ui_show(START_PAGES[g_settings.start_screen]);

    // Przełączenie bez animacji LVGL - przejście robi podświetlenie (patrz splash_end)
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(scr);
    lv_obj_del(old);
    lv_timer_create(refresh_cb, UI_REFRESH_MS, NULL);
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

static void splash_end(lv_timer_t *) {
    backlight_fade(g_settings.brightness, 0, 300, splash_dark);
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

    // Oś czasu: BMW rozjaśnia się (0-0.6 s), gaśnie (1.6-2.0 s),
    // Diamel rozjaśnia się (2.0-2.7 s), przejście do stron od 4.3 s
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
